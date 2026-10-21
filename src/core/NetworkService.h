// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/PortMapper.h"
#include "slsk/Client.h"
#include "slsk/DistributedNode.h"
#include "slsk/PeerNetwork.h"
#include "slsk/PermitSource.h"
#include "slsk/ServerSession.h"
#include "slsk/TokenGenerator.h"
#include "slsk/TransferManager.h"

#include <QByteArray>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>

class QTimer;

namespace selecta::governor {
class Governor;
}
namespace selecta::meta {
class TokenStore;
}

namespace selecta::core {

class ActivityLog;

// The Soulseek account and server (Settings → Network). The password lives in the token store, never here.
struct NetworkConfig
{
    QString host = QStringLiteral("127.0.0.1"); // a local test server unless the build allows the live network
    quint16 port = 2242;
    QString username;
    quint16 listenPort = 2234; // where peers connect to us
    bool connectAtLaunch = true;
    bool liveAllowedByUser = false; // SAFE-3: the run-time half of "may this build reach the live network"
    bool portMapping = true;        // NET-3: open listenPort on the router (NAT-PMP, UPnP)

    bool operator==(const NetworkConfig &) const = default;
};

struct NetworkStatus
{
    enum class State {
        NotConfigured, // no username or password yet
        Offline,       // disconnected by the user
        Connecting,    // resolving, connecting or logging in
        Online,
        Reconnecting, // waiting before the next attempt (ReconnectPolicy)
        NeedsUser,    // won't retry on its own: rejected login, relogged elsewhere, ban notice, refused server…
    };
    State state = State::NotConfigured;
    QString username;
    QString server;       // "127.0.0.1:2242"
    QString detail;       // what happened, in words
    qint64 retryAtMs = 0; // Reconnecting: when the next attempt is due (Unix time, ms)
    bool listening = false;
    quint16 listenPort = 0;
    PortMappingStatus portMapping; // NET-3: Unavailable unless enablePortMapping() was called
    int peersReachedUs = 0;        // distinct peers that connected to our listening port this session
};

// slsk's PermitSource on the Governor: one PeerLookup permit per new outbound peer connection, delivered on a later
// event-loop turn as slsk asks (never from inside requestPeerLookup()). Each decision goes to the activity log with its
// purpose. Engine thread; the governor and the activity log outlive it.
class GovernorPermits final : public slsk::PermitSource
{
public:
    GovernorPermits(governor::Governor &governor, ActivityLog &activity, QObject *context);

    void requestPeerLookup(const QString &username, slsk::ConnectPurpose purpose, Callback deliver) override;

private:
    governor::Governor &m_governor;
    ActivityLog &m_activity;
    QPointer<QObject> m_context;
};

// The connection to the Soulseek network on the engine thread: the server session (login, keepalive, reconnecting
// through the governor's ReconnectPolicy), the peer network with its listening port, and the search tokens. Every
// budgeted step goes through the Governor (ADR-006): the login, each new peer connection (as slsk's PermitSource) and,
// through the callers, every search. Peer sockets are capped below the process's open-file limit as it is when the
// service is made (governor::limits::socketCapFor()), so a burst of them can't take the descriptors the rest of the app
// needs (P-022): raise the limit first (base::raiseOpenFileLimit()).
//
// Threading: engine thread. Ownership: owns the slsk components; the Governor, activity log and token store outlive it.
class NetworkService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;

    // The token-store account holding a Soulseek password.
    static QString passwordAccount(const QString &username);

    NetworkService(governor::Governor &governor, ActivityLog &activity, meta::TokenStore &tokens, WallClock now = {},
                   QObject *parent = nullptr);
    ~NetworkService() override;
    NetworkService(const NetworkService &) = delete;
    NetworkService &operator=(const NetworkService &) = delete;
    NetworkService(NetworkService &&) = delete;
    NetworkService &operator=(NetworkService &&) = delete;

    // Lets this run open the listening port on the router (NET-3), as NetworkConfig::portMapping says. Without it the
    // router is never touched (tests, isolated runs). Call before start().
    void enablePortMapping(PortMapper::Options options);
    // Loads the saved password for the configured user and connects if connectAtLaunch is set.
    void start(const NetworkConfig &config);
    // New settings; reconnects when the server, account or port changed while connected.
    void setConfig(const NetworkConfig &config);
    // Saves the password (empty: forget it) and connects with it.
    void setPassword(const QString &password);
    // The user asked to connect: lifts a "needs you" block and tries now (within the governor's limits).
    void connectNow();
    // The user asked to go offline: no reconnecting until connectNow().
    void disconnectNow();

    NetworkStatus status() const { return m_status; }
    bool isOnline() const { return m_server->isLoggedIn(); }
    slsk::ServerSession &server() { return *m_server; }
    slsk::PeerNetwork &peers() { return *m_peers; }
    // Downloads and uploads over the peer network (TransferService drives them).
    slsk::TransferManager &transfers() { return *m_transfers; }

    // Sends a search with a Search permit from the governor and opens its token for results; returns the token, or 0
    // when offline (the permit is dropped).
    quint32 search(const QString &query, governor::Permit permit);
    // The same for a wishlist search (SRCH-14): the server's own message for searches that repeat in the background.
    quint32 searchWishlist(const QString &query, governor::Permit permit);
    // A private message (NET-7), with a PrivateMessage permit. False when not connected or the text can't go.
    bool sendPrivateMessage(const QString &username, const QString &text, governor::Permit permit);
    // Stops accepting results for this search.
    void stopSearch(quint32 token);
    // How often the server wants wishlist searches to run (NET-6); 12 minutes until it says.
    std::chrono::seconds wishlistInterval() const { return m_wishlistInterval; }

    // The PeerLookup permits behind every new outbound peer connection.
    slsk::PermitSource &permits() { return *m_permits; }

    // --- sharing (SHARE-2) ---
    // Our place in the distributed search network: without a parent, nobody's searches reach us.
    slsk::DistributedNode &distributed() { return *m_distributed; }
    QString username() const { return m_server->username(); }
    // Sends our answer to a search that reached us (only files we found for it), with our upload state. False when
    // there is nothing to send or no session.
    bool answerSearch(const slsk::IncomingSearch &search, const QList<slsk::FileEntry> &results,
                      slsk::Client::UploadAvailability uploads);
    // Tells the server what we share (folders and files), now and after every login.
    void setSharedCounts(quint32 folders, quint32 files) { m_server->setSharedCounts(folders, files); }

Q_SIGNALS:
    void statusChanged(const selecta::core::NetworkStatus &status);
    // `address`: where the results came from (null when unknown), for the peer's country (P-002).
    void searchResponseReceived(quint32 token, const QString &username,
                                const selecta::slsk::peer::FileSearchResponse &response, const QHostAddress &address);
    // A search that reached us, from the server or the distributed network (ShareService answers it).
    void searchRequestReceived(const selecta::slsk::IncomingSearch &search);
    // The server's list of privileged users (they go first in the upload queue).
    void privilegedUsersReceived(const QStringList &users);
    void wishlistIntervalChanged(std::chrono::seconds interval);
    // A private message from a user (NET-7); the server's own messages are notices, not these.
    void privateMessageReceived(const selecta::slsk::fromserver::PrivateMessage &message);

private Q_SLOTS:
    void hostLookedUp(const QHostInfo &info);

private:
    void attempt();
    void abortLookup();
    void connectTo(const QHostAddress &address);
    void loggedIn(const slsk::LoginInfo &info);
    void ended(slsk::ServerSession::EndReason reason, const QString &detail);
    void scheduleRetry(qint64 delayMs, const QString &detail);
    void setState(NetworkStatus::State state, const QString &detail = {});
    void ensureListening();
    void updatePortMapping();
    // The port peers should dial: the router's external port when it mapped another one.
    quint16 advertisedPort() const;
    void log(const QString &outcome, const QString &explanation);

    governor::Governor &m_governor;
    ActivityLog &m_activity;
    std::chrono::seconds m_wishlistInterval{720};
    meta::TokenStore &m_tokens;
    WallClock m_now;
    std::unique_ptr<slsk::ServerSession> m_server;
    std::unique_ptr<GovernorPermits> m_permits;
    std::unique_ptr<slsk::PeerNetwork> m_peers;
    std::unique_ptr<slsk::TransferManager> m_transfers; // uses the peers and the session
    std::unique_ptr<slsk::DistributedNode> m_distributed;
    slsk::TokenGenerator m_searchTokens;
    QSet<quint32> m_openSearches;
    QTimer *m_retryTimer = nullptr;
    NetworkConfig m_config;
    QString m_password;           // in memory only; never logged
    bool m_wanted = false;        // the user wants to be online (not disconnected by hand)
    int m_lookupId = -1;          // the host lookup in flight, or -1
    int m_passwordGeneration = 0; // drops a stored password read that a typed password replaced
    NetworkStatus m_status;
    std::unique_ptr<PortMapper> m_portMapper;
    QSet<QString> m_reachedBy; // peers that connected to our listening port (bounded: kReachedByMax)
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::NetworkStatus)
