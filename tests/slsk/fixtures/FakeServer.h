// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Frame.h"
#include "slsk/ServerMessages.h"

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QStringList>

#include <map>
#include <optional>

class QTcpServer;
class QTcpSocket;

namespace selecta::slsk::testing {

// An in-process, scriptable Soulseek server on a loopback port, for tests only. It models the official server where
// clients depend on it (login replies and the welcome burst, Relogged on a duplicate login, address lookups,
// ConnectToPeer relaying and CantConnectToPeer, private messages, user and room searches, rooms) and the distributed
// network that Soulfind lacks (tests inject PossibleParents, EmbeddedMessage, ResetDistributed with sendTo()). Like
// Soulfind, it drops a client that sends a frame over 8 KiB and ignores anything sent before the login.
//
// Virtual users stand in for FakePeers: they have an address but no server connection, and the server calls them for
// the indirect connection requests and searches meant for them.
//
// Threading: lives on the test's thread. Ownership: owns its listener and client sockets.
class FakeServer : public QObject
{
    Q_OBJECT

public:
    enum class LoginBehaviour {
        Accept,
        Reject,         // a LoginReply failure with the configured reason, then close
        Silent,         // never answer (a banned account)
        CloseOnLogin,   // close as soon as the login request arrives (an IP throttle)
        CloseOnConnect, // close right after accepting the connection
    };

    enum class SearchRouting {
        None,
        Direct,                  // FileSearch reaches every other user as code 26
        DirectIncludingSearcher, // Soulfind: the searcher gets its own search back too
    };

    class VirtualUser
    {
    public:
        virtual ~VirtualUser() = default;
        virtual void connectToPeerRequested(const fromserver::ConnectToPeerRequest &request) = 0;
        virtual void searchRequested(const fromserver::SearchRequest &request) = 0;
        virtual void cantConnectToPeer(quint32 token) = 0;
    };

    // What the server sends right after a successful login (official order, protocol §4.3).
    struct Welcome
    {
        QList<RoomListEntry> rooms;
        std::optional<quint32> parentMinSpeed = 1;
        std::optional<quint32> parentSpeedRatio = 50;
        quint32 wishlistInterval = 720;
        QStringList privilegedUsers;
        QStringList excludedPhrases;
        QList<fromserver::PrivateMessage> queuedMessages;
    };

    static constexpr qsizetype kClientFrameMax = 8192; // Soulfind's inbound limit

    // A connected client.
    struct Client
    {
        QTcpSocket *socket = nullptr;
        FrameReader reader{CodeWidth::Word, kClientFrameMax};
        QString username;
        bool loggedIn = false;
        quint32 listenPort = 0;
        qint32 status = 2;
        UserStats stats;
        bool haveNoParent = true;
        quint32 branchLevel = 0;
        QString branchRoot;
        bool acceptChildren = false;
        QList<ToServerMessage> received; // decoded, in order, after login
        QList<quint32> codesBeforeLogin; // codes of anything sent before the login request was answered
    };

    explicit FakeServer(QObject *parent = nullptr);
    ~FakeServer() override;
    FakeServer(const FakeServer &) = delete;
    FakeServer &operator=(const FakeServer &) = delete;

    bool start();
    QHostAddress address() const;
    quint16 port() const;

    void setLoginBehaviour(LoginBehaviour behaviour, const QString &reason = {}, const QString &detail = {});
    void setGreeting(const QString &greeting) { m_greeting = greeting; }
    void setWelcome(const Welcome &welcome) { m_welcome = welcome; }
    void setSearchRouting(SearchRouting routing) { m_routing = routing; }

    void addVirtualUser(const QString &username, const QHostAddress &address, quint16 port, VirtualUser *user);
    void removeVirtualUser(const QString &username);
    void setUserStats(const QString &username, const UserStats &stats);

    // Sends to a logged-in client; false if there is none by that name.
    bool sendTo(const QString &username, const FromServerMessage &message);
    bool sendRawTo(const QString &username, const QByteArray &bytes);
    void disconnectClient(const QString &username);
    // A virtual user's indirect connection request reaching a client; its address is the virtual user's.
    bool relayConnectToPeer(const QString &fromVirtual, const QString &toClient, ConnectionType type, quint32 token);
    bool relayCantConnectToPeer(quint32 token, const QString &toClient);
    bool sendPrivateMessage(const QString &from, const QString &toClient, const QString &text);

    QStringList loggedInUsers() const;
    bool isLoggedIn(const QString &username) const;
    const Client *client(const QString &username) const;
    QList<ToServerMessage> received(const QString &username) const;
    // Codes of everything any connection sent before its login was answered (all connections, logged in or not).
    QList<quint32> codesReceivedBeforeLogin() const;
    int connectionCount() const { return static_cast<int>(m_clients.size()); }

    template <typename M>
    QList<M> receivedOf(const QString &username) const
    {
        QList<M> matching;
        const QList<ToServerMessage> all = received(username);
        for (const ToServerMessage &message : all) {
            if (const M *m = std::get_if<M>(&message)) {
                matching.append(*m);
            }
        }
        return matching;
    }

Q_SIGNALS:
    void messageReceived(const QString &username, const selecta::slsk::ToServerMessage &message);
    void clientLoggedIn(const QString &username);
    void clientDisconnected(const QString &username);

private:
    struct Handler;
    friend struct Handler;

    struct VirtualEntry
    {
        QHostAddress address;
        quint16 port = 0;
        VirtualUser *user = nullptr;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);
    void handleLogin(Client &client, const toserver::Login &login);
    void handle(Client &client, const ToServerMessage &message);
    void routeSearch(const Client &from, quint32 token, const QString &query);
    void deliverSearch(const QString &to, const fromserver::SearchRequest &request);
    void dropClient(QTcpSocket *socket);
    Client *findLoggedIn(const QString &username);
    bool write(Client &client, const QByteArray &bytes);
    template <typename M>
    bool writeMessage(Client &client, const M &message)
    {
        return write(client, frame(message));
    }
    UserStats statsOf(const QString &username) const;

    QTcpServer *m_listener;
    std::map<QTcpSocket *, Client> m_clients; // node-based: references survive other clients leaving
    QHash<QString, VirtualEntry> m_virtualUsers;
    QHash<QString, UserStats> m_stats;
    QHash<QString, QStringList> m_rooms; // room → members, in join order
    LoginBehaviour m_loginBehaviour = LoginBehaviour::Accept;
    QString m_rejectReason;
    QString m_rejectDetail;
    QString m_greeting = QStringLiteral("Welcome to the fake Soulseek server");
    Welcome m_welcome;
    SearchRouting m_routing = SearchRouting::Direct;
    quint32 m_nextMessageId = 1;
};

} // namespace selecta::slsk::testing
