// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "slsk/Frame.h"
#include "slsk/ServerMessages.h"
#include "slsk/Types.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class QTcpSocket;
class QTimer;

namespace selecta::slsk {

// What the server said when the login succeeded.
struct LoginInfo
{
    QString username;
    QString greeting;        // message of the day
    QHostAddress ownAddress; // our public IPv4 address as the server sees it
    bool isSupporter = false;

    friend bool operator==(const LoginInfo &, const LoginInfo &) = default;
};

// A notice from the server: a private message from the pseudo-user `server`, or a global AdminMessage. The 30-minute
// flood ban and the "connection restricted" warning (old client version) are recognised in both channels (etiquette
// §4.1–4.2).
struct ServerNotice
{
    enum class Kind {
        Ban,        // "You have been banned for 30 minutes…": stop all automatic activity
        Restricted, // "Your connection is restricted: You cannot search or chat…"
        Other,
    };
    enum class Source {
        PrivateMessage,
        AdminMessage,
    };

    Kind kind = Kind::Other;
    Source source = Source::PrivateMessage;
    QString text;

    friend bool operator==(const ServerNotice &, const ServerNotice &) = default;
};

// Recognises the ban and restriction texts. Pure.
ServerNotice::Kind classifyServerNotice(const QString &text);

// Why an outgoing chat line or room name can't be sent. Validation is pure and cheap: core/ calls it before asking the
// governor, so a refused line never costs a permit.
enum class ChatTextError {
    None,
    Empty,
    TooLong,         // over protocol::kChatLineMaxBytes of UTF-8
    InvalidRoomName, // room names: 1–24 printable ASCII characters, no leading, trailing or double spaces
};

ChatTextError validateRoomName(const QString &room);
// Replaces CR and LF with spaces (the server refuses newlines) and checks the length.
ChatTextError prepareChatLine(QString &text);

// The connection to the Soulseek server (references/soulseek-protocol.md §4, §9.1, §11).
//
// Lifecycle: connectToServer() opens the TCP connection (needs a Login permit), login() sends the login request as soon
// as it is up and nothing else goes out until the reply arrives. After a successful reply the session sends the
// post-login burst (listen port, status, share counts, privileges check, a watch on our own user, room invitations
// off), emits loggedIn(), and keeps the connection alive with TCP keepalive and a ServerPing every 5 minutes.
//
// Every session ends exactly once, with a typed reason (ended()), and goes back to Disconnected. The session never
// reconnects on its own: core/ asks the governor, then calls connectToServer() again. EndReason maps one-to-one onto
// governor::ReconnectPolicy::Event (see EndReason).
//
// Threading: lives on the engine thread and is used only from it. Nothing blocks; every wait has a timeout.
// Ownership: a QObject; owns its socket and timers. Failure modes: socket errors and timeouts end the session with a
// reason; a malformed message is logged and dropped (selecta.slsk.server); an oversize or truncated frame ends the
// session with ProtocolViolation.
class ServerSession : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting, // TCP connect in progress
        Connected,  // TCP up, login() not called yet
        LoggingIn,  // login request sent, waiting for the reply
        LoggedIn,
    };
    Q_ENUM(State)

    // How a session ended. The comment names the matching governor::ReconnectPolicy::Event.
    enum class EndReason {
        None,              // the session hasn't ended (or never started)
        LoggedOut,         // disconnectFromServer(): the user's choice, nothing to report
        Relogged,          // Relogged: the account logged in elsewhere; never reconnect automatically
        LoginRejected,     // LoginRejected: the server refused the login (loginRejection() has the reason)
        ServerFull,        // ServerFull: the server refused the login with SVRFULL
        LoginTimeout,      // LoginTimeout: no reply within 30 s; possibly a silent ban
        InstantEof,        // InstantEof: the server closed the connection before replying; possibly an IP throttle
        BanNotice,         // BanNotice: the server sent its ban message (then usually disconnects)
        ConnectionLost,    // ConnectionLost: a logged-in session dropped
        ConnectFailed,     // no counterpart: the TCP connection couldn't be opened (refused, unreachable, timeout)
        ProtocolViolation, // no counterpart: an oversize or impossible frame from the server; don't retry blindly
    };
    Q_ENUM(EndReason)

    struct LoginRejection
    {
        QString reason; // INVALIDUSERNAME, EMPTYPASSWORD, INVALIDPASS, INVALIDVERSION, SVRFULL, SVRPRIVATE, …
        QString detail; // with INVALIDUSERNAME: "Nick too long." etc.
    };

    // Timers default to protocol.h's values; tests shorten them.
    struct Timeouts
    {
        std::chrono::milliseconds connect = protocol::kServerConnectTimeout;
        std::chrono::milliseconds loginReply = protocol::kLoginReplyTimeout;
        std::chrono::milliseconds ping = protocol::kServerPingInterval;
    };

    explicit ServerSession(QObject *parent = nullptr);
    ~ServerSession() override;
    ServerSession(const ServerSession &) = delete;
    ServerSession &operator=(const ServerSession &) = delete;

    void setTimeouts(const Timeouts &timeouts) { m_timeouts = timeouts; }

    // --- connection --------------------------------------------------------------------------------------------------

    // Opens the TCP connection to `address`:`port`. The caller has already checked governor::addressAllowed(); `login`
    // must be a Login permit. Returns false, consuming nothing, when the session isn't Disconnected or the permit is
    // invalid. A failed connection ends the session with ConnectFailed.
    bool connectToServer(const QHostAddress &address, quint16 port, governor::Permit login);
    // Sends the login request as soon as the connection is up (right away when Connected). Returns false when there is
    // no connection in progress, a login was already sent, or the username is empty.
    bool login(const QString &username, const QString &password);
    // Ends the session with LoggedOut (no signal is lost: ended() is emitted before this returns, if a session was
    // running).
    void disconnectFromServer();

    State state() const { return m_state; }
    bool isLoggedIn() const { return m_state == State::LoggedIn; }
    // The reason of the last session that ended; None while one runs.
    EndReason endReason() const { return m_endReason; }
    LoginRejection loginRejection() const { return m_rejection; }
    // Valid while logged in.
    QString username() const { return m_username; }
    LoginInfo loginInfo() const { return m_loginInfo; }

    // --- values for the post-login burst; changing them while logged in sends the update -----------------------------
    void setListenPort(quint16 port);
    void setSharedCounts(quint32 folders, quint32 files);
    void setStatus(UserStatus status); // Online or Away; other values are ignored

    // --- budgeted sends ----------------------------------------------------------------------------------------------
    // Each consumes its permit. False (permit dropped) when not logged in or the input is invalid.
    bool fileSearch(quint32 token, const QString &query, governor::Permit search);
    bool userSearch(const QString &username, quint32 token, const QString &query, governor::Permit search);
    bool wishlistSearch(quint32 token, const QString &query, governor::Permit search);
    // CR/LF become spaces; a line over protocol::kChatLineMaxBytes is refused (see prepareChatLine()).
    bool sendPrivateMessage(const QString &username, const QString &text, governor::Permit privateMessage);
    // Only in a room we joined. The server echoes our own line back as a roomMessage().
    bool sayInRoom(const QString &room, const QString &text, governor::Permit roomMessage);
    bool getPeerAddress(const QString &username, governor::Permit peerLookup);
    // The indirect half of a connection attempt (ConnectToPeer) plus, unless one is already outstanding for this
    // user, the address lookup for the direct half (GetPeerAddress): one PeerLookup permit for both (protocol §3.2).
    bool requestPeerConnection(quint32 token, const QString &username, ConnectionType type,
                               governor::Permit peerLookup);
    // ConnectToPeer only, for an attempt whose address is already known (distributed parent candidates).
    bool requestIndirectConnection(quint32 token, const QString &username, ConnectionType type,
                                   governor::Permit peerLookup);

    // --- unbudgeted sends --------------------------------------------------------------------------------------------
    // core/ paces the ones the etiquette note limits (joins, room lists). False when not logged in.
    bool watchUser(const QString &username);
    bool unwatchUser(const QString &username);
    bool requestUserStatus(const QString &username);
    bool requestUserStats(const QString &username);
    bool checkPrivileges();
    bool sendUploadSpeed(quint32 bytesPerSecond);
    bool requestRoomList();
    // Joins a public room (never creates a private one). Refused for invalid names (validateRoomName()).
    bool joinRoom(const QString &room);
    bool leaveRoom(const QString &room);
    QStringList joinedRooms() const;
    // Our answer when we couldn't answer someone's indirect connection request.
    bool cantConnectToPeer(quint32 token, const QString &username);
    // Distributed-network reports (DistributedNode).
    bool sendHaveNoParent(bool noParent);
    bool sendBranchLevel(quint32 level);
    bool sendBranchRoot(const QString &root);
    bool sendAcceptChildren(bool accept);

Q_SIGNALS:
    void stateChanged(selecta::slsk::ServerSession::State state);
    void loggedIn(const selecta::slsk::LoginInfo &info);
    // Emitted once per session, after which state() is Disconnected. `detail` is human-readable (socket error text,
    // rejection reason).
    void ended(selecta::slsk::ServerSession::EndReason reason, const QString &detail);

    // Server notices (PMs from `server`, AdminMessage), ban and restriction recognised.
    void serverNotice(const selecta::slsk::ServerNotice &notice);
    // Every private message, including those from `server`. Already acknowledged to the server.
    void privateMessageReceived(const selecta::slsk::fromserver::PrivateMessage &message);
    // A user or room search aimed at us (code 26). Not filtered: searches from ourselves arrive too.
    void searchRequestReceived(const selecta::slsk::fromserver::SearchRequest &request);

    void userStatusReceived(const selecta::slsk::fromserver::UserStatusUpdate &update);
    void userStatsReceived(const selecta::slsk::fromserver::UserStatsUpdate &update);
    void watchUserReplyReceived(const selecta::slsk::fromserver::WatchUserReply &reply);
    void privilegedUsersReceived(const QStringList &users);
    void excludedSearchPhrasesReceived(const QStringList &phrases);
    void wishlistIntervalReceived(quint32 seconds);
    void privilegesLeftReceived(quint32 seconds);

    // Connection brokering (PeerNetwork).
    void peerAddressReceived(const selecta::slsk::fromserver::PeerAddress &address);
    void connectToPeerRequested(const selecta::slsk::fromserver::ConnectToPeerRequest &request);
    void cantConnectToPeerReceived(quint32 token);

    // Distributed network (DistributedNode).
    void parentMinSpeedReceived(quint32 speed);
    void parentSpeedRatioReceived(quint32 ratio);
    void possibleParentsReceived(const QList<selecta::slsk::ParentCandidate> &candidates);
    void embeddedMessageReceived(quint8 distribCode, const QByteArray &payload);
    void resetDistributedReceived();

    // Rooms (D32). Events for rooms we haven't joined are dropped; an unsolicited JoinRoom reply is answered with
    // LeaveRoom.
    void roomListReceived(const selecta::slsk::fromserver::RoomList &list);
    void roomJoined(const selecta::slsk::fromserver::RoomJoined &room);
    void roomLeft(const QString &room);
    void roomUserJoined(const selecta::slsk::fromserver::RoomUserJoined &event);
    void roomUserLeft(const selecta::slsk::fromserver::RoomUserLeft &event);
    void roomMessageReceived(const selecta::slsk::fromserver::RoomMessage &message);
    void roomJoinFailed(const QString &room); // CantCreateRoom: the name belongs to a private room

private:
    enum class RoomState {
        Joining,
        Joined,
        Leaving,
    };

    void setState(State state);
    void end(EndReason reason, const QString &detail);
    void onConnected();
    void onReadyRead();
    void onSocketClosed(const QString &detail);
    void onLoginTimeout();
    void sendLogin();
    void sendPostLoginBurst();
    template <typename Message>
    bool send(const Message &message);
    bool sendIfLoggedIn(const QByteArray &frame);
    bool consumePermit(governor::Permit &permit, governor::Operation operation);
    void handleFrame(const Frame &frame);
    void handleLoginReply(const fromserver::LoginReply &reply);
    void handlePrivateMessage(const fromserver::PrivateMessage &message);
    void handleRoomJoined(const fromserver::RoomJoined &room);
    void handleRoomLeft(const fromserver::RoomLeft &room);
    void handleRoomEvent(const FromServerMessage &message);
    void dispatch(FromServerMessage &&message);
    void enableKeepAlive();

    Timeouts m_timeouts;
    std::unique_ptr<QTcpSocket> m_socket;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_loginTimer = nullptr;
    QTimer *m_pingTimer = nullptr;
    FrameReader m_reader;
    State m_state = State::Disconnected;
    EndReason m_endReason = EndReason::None;
    LoginRejection m_rejection;
    LoginInfo m_loginInfo;
    QString m_username;
    QString m_password;
    bool m_loginRequested = false;
    bool m_banNoticeSeen = false;
    quint16 m_listenPort = protocol::kDefaultListenPort;
    quint32 m_sharedFolders = 0;
    quint32 m_sharedFiles = 0;
    UserStatus m_status = UserStatus::Online;
    QSet<QString> m_pendingAddressLookups; // GetPeerAddress sent, reply not yet seen
    QHash<QString, RoomState> m_rooms;
};

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::LoginInfo)
Q_DECLARE_METATYPE(selecta::slsk::ServerNotice)
