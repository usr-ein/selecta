// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/ServerSession.h"

#include "slsk/Logging.h"
#include "slsk/MetaTypes.h"

#include <QMetaEnum>
#include <QTcpSocket>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <utility>
#include <variant>

using namespace Qt::StringLiterals;

namespace selecta::slsk {
namespace {

// Bytes the socket may hold before we read them; we always drain it at once, so this only bounds a burst.
constexpr qint64 kSocketReadBuffer = qint64{1024} * 1024;

// The usual visitor idiom: one call operator per message type.
template <class... Ts>
struct Overloaded : Ts... // NOLINT(misc-multiple-inheritance): stateless lambdas, no diamond
{
    using Ts::operator()...;
};

bool isPrintableAscii(QChar c)
{
    return c.unicode() >= 0x20 && c.unicode() <= 0x7E;
}

} // namespace

ServerNotice::Kind classifyServerNotice(const QString &text)
{
    if (text.contains(u"You have been banned"_s, Qt::CaseInsensitive)) {
        return ServerNotice::Kind::Ban;
    }
    if (text.contains(u"connection is restricted"_s, Qt::CaseInsensitive)) {
        return ServerNotice::Kind::Restricted;
    }
    return ServerNotice::Kind::Other;
}

ChatTextError validateRoomName(const QString &room)
{
    if (room.isEmpty() || room.size() > protocol::kRoomNameMaxChars) {
        return ChatTextError::InvalidRoomName;
    }
    if (!std::ranges::all_of(room, isPrintableAscii)) {
        return ChatTextError::InvalidRoomName;
    }
    if (room.startsWith(u' ') || room.endsWith(u' ') || room.contains(u"  "_s)) {
        return ChatTextError::InvalidRoomName;
    }
    return ChatTextError::None;
}

ChatTextError prepareChatLine(QString &text)
{
    // As Nicotine+ does: the server refuses newlines.
    text.remove(u'\r');
    text.replace(u'\n', u' ');
    if (text.isEmpty()) {
        return ChatTextError::Empty;
    }
    if (text.toUtf8().size() > protocol::kChatLineMaxBytes) {
        return ChatTextError::TooLong;
    }
    return ChatTextError::None;
}

ServerSession::ServerSession(QObject *parent)
    : QObject(parent)
    , m_connectTimer(new QTimer(this))
    , m_loginTimer(new QTimer(this))
    , m_pingTimer(new QTimer(this))
    , m_reader(CodeWidth::Word, protocol::kServerFrameMax)
{
    registerMetaTypes();
    m_connectTimer->setSingleShot(true);
    m_loginTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this,
            [this] { end(EndReason::ConnectFailed, u"connection timed out"_s); });
    connect(m_loginTimer, &QTimer::timeout, this, &ServerSession::onLoginTimeout);
    connect(m_pingTimer, &QTimer::timeout, this, [this] { send(toserver::ServerPing{}); });
}

ServerSession::~ServerSession()
{
    // No signals from a half-destroyed object: drop the socket quietly.
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

void ServerSession::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(state);
}

bool ServerSession::connectToServer(const QHostAddress &address, quint16 port, governor::Permit login)
{
    if (m_state != State::Disconnected) {
        qCWarning(lcSlskServer) << "connectToServer: a session is already running";
        return false;
    }
    if (!consumePermit(login, governor::Operation::Login)) {
        return false;
    }
    m_endReason = EndReason::None;
    m_rejection = {};
    m_loginInfo = {};
    m_banNoticeSeen = false;
    m_reader = FrameReader(CodeWidth::Word, protocol::kServerFrameMax);

    m_socket = new QTcpSocket(this);
    m_socket->setReadBufferSize(kSocketReadBuffer);
    connect(m_socket, &QTcpSocket::connected, this, &ServerSession::onConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &ServerSession::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this,
            [this] { onSocketClosed(u"the server closed the connection"_s); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        const QString detail = m_socket != nullptr ? m_socket->errorString() : QString();
        if (m_state == State::Connecting && error != QAbstractSocket::RemoteHostClosedError) {
            end(EndReason::ConnectFailed, detail);
        } else {
            onSocketClosed(detail);
        }
    });
    setState(State::Connecting);
    m_connectTimer->start(m_timeouts.connect);
    qCInfo(lcSlskServer).noquote() << "connecting to" << address.toString() << port;
    m_socket->connectToHost(address, port);
    return true;
}

bool ServerSession::login(const QString &username, const QString &password)
{
    if ((m_state != State::Connecting && m_state != State::Connected) || m_loginRequested || username.isEmpty()) {
        return false;
    }
    m_username = username;
    m_password = password;
    m_loginRequested = true;
    if (m_state == State::Connected) {
        sendLogin();
    }
    return true;
}

void ServerSession::disconnectFromServer()
{
    if (m_state != State::Disconnected) {
        end(EndReason::LoggedOut, {});
    }
}

void ServerSession::onConnected()
{
    m_connectTimer->stop();
    enableKeepAlive();
    setState(State::Connected);
    if (m_loginRequested) {
        sendLogin();
    } else {
        // Waiting for the caller: bounded too (the server drops unauthenticated sockets anyway).
        m_loginTimer->start(m_timeouts.loginReply);
    }
}

void ServerSession::sendLogin()
{
    // Nothing else goes out before the reply (Soulfind leaves earlier messages unread).
    send(toserver::Login::make(m_username, m_password));
    m_password.clear(); // not kept longer than needed
    setState(State::LoggingIn);
    m_loginTimer->start(m_timeouts.loginReply);
}

void ServerSession::onLoginTimeout()
{
    if (m_state == State::Connected) {
        // A caller error, not a network condition: nothing to retry.
        qCWarning(lcSlskServer) << "connected, but login() was never called";
        end(EndReason::LoggedOut, u"login() was never called"_s);
        return;
    }
    end(EndReason::LoginTimeout, u"no reply to the login request"_s);
}

void ServerSession::onReadyRead()
{
    if (m_socket == nullptr) {
        return;
    }
    QTcpSocket *const socket = m_socket;
    m_reader.append(socket->readAll());
    while (m_socket == socket) {
        const std::optional<Frame> frame = m_reader.next();
        if (!frame) {
            break;
        }
        handleFrame(*frame);
    }
    if (m_socket == socket && m_reader.hasError()) {
        end(EndReason::ProtocolViolation, QString::fromLatin1(frameErrorName(m_reader.error())));
    }
}

void ServerSession::onSocketClosed(const QString &detail)
{
    switch (m_state) {
    case State::Disconnected:
        return;
    case State::Connecting:
        end(EndReason::ConnectFailed, detail);
        return;
    case State::Connected:
    case State::LoggingIn:
        // Closed before any reply: an IP throttle looks exactly like this (protocol §4.2).
        end(EndReason::InstantEof, detail);
        return;
    case State::LoggedIn:
        end(m_banNoticeSeen ? EndReason::BanNotice : EndReason::ConnectionLost, detail);
        return;
    }
}

void ServerSession::end(EndReason reason, const QString &detail)
{
    if (m_state == State::Disconnected && m_socket == nullptr) {
        return;
    }
    m_connectTimer->stop();
    m_loginTimer->stop();
    m_pingTimer->stop();
    if (m_socket != nullptr) {
        QTcpSocket *const socket = std::exchange(m_socket, nullptr);
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater(); // we may be inside one of its signals
    }
    m_reader = FrameReader(CodeWidth::Word, protocol::kServerFrameMax);
    m_loginRequested = false;
    m_password.clear();
    m_pendingAddressLookups.clear();
    m_rooms.clear();
    m_endReason = reason;
    if (reason == EndReason::LoggedOut) {
        qCInfo(lcSlskServer) << "session ended: logged out";
    } else {
        qCWarning(lcSlskServer).noquote()
            << "session ended:" << QMetaEnum::fromType<EndReason>().valueToKey(static_cast<int>(reason)) << detail;
    }
    setState(State::Disconnected);
    Q_EMIT ended(reason, detail);
}

void ServerSession::enableKeepAlive()
{
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
#ifdef Q_OS_UNIX
    // Detect a dead server connection within about two minutes (protocol §12.4). Best effort: a platform without
    // these options keeps the system defaults, and ServerPing still runs.
    const auto fd = static_cast<int>(m_socket->socketDescriptor());
    if (fd < 0) {
        return;
    }
    const int idle = static_cast<int>(protocol::kKeepAliveIdle.count());
    const int interval = static_cast<int>(protocol::kKeepAliveInterval.count());
    const int probes = protocol::kKeepAliveProbes;
#ifdef TCP_KEEPALIVE
    const int idleOption = TCP_KEEPALIVE; // macOS name
#else
    const int idleOption = TCP_KEEPIDLE;
#endif
    if (setsockopt(fd, IPPROTO_TCP, idleOption, &idle, sizeof idle) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof probes) != 0) {
        qCDebug(lcSlskServer) << "could not tune TCP keepalive; using the system defaults";
    }
#endif
}

template <typename Message>
bool ServerSession::send(const Message &message)
{
    if (m_socket == nullptr) {
        return false;
    }
    const QByteArray bytes = frame(message);
    return m_socket->write(bytes) == bytes.size();
}

template <typename Message>
bool ServerSession::sendIfLoggedIn(const Message &message)
{
    return m_state == State::LoggedIn && send(message);
}

bool ServerSession::consumePermit(governor::Permit &permit, governor::Operation operation)
{
    // Checked first so a wrong permit is refused, not asserted on.
    if (!permit.isValid() || permit.operation() != operation) {
        qCWarning(lcSlskServer) << "refused: permit missing or for another operation";
        return false;
    }
    return permit.consume(operation);
}

void ServerSession::sendPostLoginBurst()
{
    send(toserver::SetWaitPort{m_listenPort, std::nullopt, std::nullopt});
    send(toserver::SetStatus{static_cast<qint32>(m_status)});
    send(toserver::SharedFoldersFiles{m_sharedFolders, m_sharedFiles});
    send(toserver::CheckPrivileges{});
    send(toserver::WatchUser{m_username}); // our own average speed: DistributedNode's child limit
    send(toserver::EnableRoomInvitations{false});
}

// --- incoming --------------------------------------------------------------------------------------------------------

void ServerSession::handleFrame(const Frame &frame)
{
    Decoded<FromServerMessage> decoded = decodeFromServer(frame.code, frame.payload);
    switch (decoded.status) {
    case DecodeStatus::Decoded:
        break;
    case DecodeStatus::Malformed:
        qCWarning(lcSlskServer) << "dropped malformed message, code" << frame.code << "size" << frame.payload.size();
        return;
    case DecodeStatus::Ignored:
        qCDebug(lcSlskServer) << "ignored message, code" << frame.code;
        return;
    case DecodeStatus::Unknown:
        qCDebug(lcSlskServer) << "skipped unknown message, code" << frame.code << "size" << frame.payload.size();
        return;
    }
    if (!decoded.message) {
        return;
    }
    if (m_state != State::LoggedIn) {
        // Before the login reply only the reply itself means anything.
        if (const auto *reply = std::get_if<fromserver::LoginReply>(&*decoded.message)) {
            handleLoginReply(*reply);
        } else {
            qCDebug(lcSlskServer) << "dropped message before login, code" << frame.code;
        }
        return;
    }
    dispatch(*decoded.message);
}

void ServerSession::handleLoginReply(const fromserver::LoginReply &reply)
{
    m_loginTimer->stop();
    if (!reply.success) {
        m_rejection = LoginRejection{reply.reason, reply.detail};
        const QString detail = reply.detail.isEmpty() ? reply.reason : reply.reason + u": "_s + reply.detail;
        end(reply.reason == u"SVRFULL"_s ? EndReason::ServerFull : EndReason::LoginRejected, detail);
        return;
    }
    m_loginInfo = LoginInfo{m_username, reply.greeting, ipv4Address(reply.ownIp), reply.isSupporter};
    setState(State::LoggedIn);
    sendPostLoginBurst();
    m_pingTimer->start(m_timeouts.ping);
    qCInfo(lcSlskServer).noquote() << "logged in as" << m_username;
    Q_EMIT loggedIn(m_loginInfo);
}

void ServerSession::dispatch(const FromServerMessage &message)
{
    std::visit(
        Overloaded{
            [](const fromserver::LoginReply &) { qCDebug(lcSlskServer) << "ignored a second login reply"; },
            [this](const fromserver::PeerAddress &m) {
                m_pendingAddressLookups.remove(m.username);
                Q_EMIT peerAddressReceived(m);
            },
            [this](const fromserver::WatchUserReply &m) { Q_EMIT watchUserReplyReceived(m); },
            [this](const fromserver::UserStatusUpdate &m) { Q_EMIT userStatusReceived(m); },
            [this](const fromserver::RoomMessage &m) {
                if (m_rooms.value(m.room) == RoomState::Joined) {
                    Q_EMIT roomMessageReceived(m);
                }
            },
            [this](const fromserver::RoomJoined &m) { handleRoomJoined(m); },
            [this](const fromserver::RoomLeft &m) { handleRoomLeft(m); },
            [this](const fromserver::RoomUserJoined &m) {
                // Our own UserJoinedRoom arrives before the JoinRoom reply: ignored until we're in.
                if (m_rooms.value(m.room) == RoomState::Joined) {
                    Q_EMIT roomUserJoined(m);
                }
            },
            [this](const fromserver::RoomUserLeft &m) {
                if (m_rooms.value(m.room) == RoomState::Joined) {
                    Q_EMIT roomUserLeft(m);
                }
            },
            [this](const fromserver::ConnectToPeerRequest &m) { Q_EMIT connectToPeerRequested(m); },
            [this](const fromserver::PrivateMessage &m) { handlePrivateMessage(m); },
            [this](const fromserver::SearchRequest &m) { Q_EMIT searchRequestReceived(m); },
            [this](const fromserver::UserStatsUpdate &m) { Q_EMIT userStatsReceived(m); },
            [this](fromserver::Relogged) { end(EndReason::Relogged, u"the account logged in elsewhere"_s); },
            [this](const fromserver::RoomList &m) { Q_EMIT roomListReceived(m); },
            [this](const fromserver::AdminMessage &m) {
                const ServerNotice notice{classifyServerNotice(m.message), ServerNotice::Source::AdminMessage,
                                          m.message};
                if (notice.kind == ServerNotice::Kind::Ban) {
                    m_banNoticeSeen = true;
                }
                Q_EMIT serverNotice(notice);
            },
            [this](const fromserver::PrivilegedUsers &m) { Q_EMIT privilegedUsersReceived(m.users); },
            [this](fromserver::ParentMinSpeed m) { Q_EMIT parentMinSpeedReceived(m.speed); },
            [this](fromserver::ParentSpeedRatio m) { Q_EMIT parentSpeedRatioReceived(m.ratio); },
            [this](fromserver::PrivilegesLeft m) { Q_EMIT privilegesLeftReceived(m.seconds); },
            [this](const fromserver::EmbeddedMessage &m) { Q_EMIT embeddedMessageReceived(m.distribCode, m.payload); },
            [this](const fromserver::PossibleParents &m) { Q_EMIT possibleParentsReceived(m.candidates); },
            [this](fromserver::WishlistInterval m) { Q_EMIT wishlistIntervalReceived(m.seconds); },
            [this](fromserver::ResetDistributed) { Q_EMIT resetDistributedReceived(); },
            [](fromserver::RoomInvitationsEnabled) {},
            [this](const fromserver::ExcludedSearchPhrases &m) { Q_EMIT excludedSearchPhrasesReceived(m.phrases); },
            [this](fromserver::CantConnectToPeer m) { Q_EMIT cantConnectToPeerReceived(m.token); },
            [this](const fromserver::CantCreateRoom &m) {
                if (m_rooms.remove(m.room) > 0) {
                    Q_EMIT roomJoinFailed(m.room);
                }
            },
        },
        message);
}

void ServerSession::handlePrivateMessage(const fromserver::PrivateMessage &message)
{
    // Acknowledged at once, or the server keeps re-sending it.
    send(toserver::MessageAcked{message.id});
    if (message.username == protocol::kServerUsername) {
        const ServerNotice notice{classifyServerNotice(message.message), ServerNotice::Source::PrivateMessage,
                                  message.message};
        if (notice.kind == ServerNotice::Kind::Ban) {
            m_banNoticeSeen = true;
        }
        Q_EMIT serverNotice(notice);
    }
    Q_EMIT privateMessageReceived(message);
}

void ServerSession::handleRoomJoined(const fromserver::RoomJoined &room)
{
    const auto it = m_rooms.constFind(room.room);
    if (it == m_rooms.constEnd()) {
        // A room we never asked for: leave it, as Nicotine+ does.
        qCInfo(lcSlskServer).noquote() << "leaving unsolicited room" << room.room;
        send(toserver::LeaveRoom{room.room});
        m_rooms.insert(room.room, RoomState::Leaving);
        return;
    }
    if (it.value() == RoomState::Leaving) {
        return; // a late reply to a join we've since withdrawn
    }
    m_rooms.insert(room.room, RoomState::Joined);
    Q_EMIT roomJoined(room);
}

void ServerSession::handleRoomLeft(const fromserver::RoomLeft &room)
{
    if (m_rooms.remove(room.room) > 0) {
        Q_EMIT roomLeft(room.room);
    }
}

// --- settings sent in the burst --------------------------------------------------------------------------------------

void ServerSession::setListenPort(quint16 port)
{
    m_listenPort = port;
    sendIfLoggedIn(toserver::SetWaitPort{port, std::nullopt, std::nullopt});
}

void ServerSession::setSharedCounts(quint32 folders, quint32 files)
{
    m_sharedFolders = folders;
    m_sharedFiles = files;
    sendIfLoggedIn(toserver::SharedFoldersFiles{folders, files});
}

void ServerSession::setStatus(UserStatus status)
{
    if (status != UserStatus::Online && status != UserStatus::Away) {
        return;
    }
    m_status = status;
    sendIfLoggedIn(toserver::SetStatus{static_cast<qint32>(status)});
}

// --- budgeted sends --------------------------------------------------------------------------------------------------

bool ServerSession::fileSearch(quint32 token, const QString &query, governor::Permit search)
{
    if (!isLoggedIn() || query.isEmpty() || !consumePermit(search, governor::Operation::Search)) {
        return false;
    }
    return send(toserver::FileSearch{token, query});
}

bool ServerSession::userSearch(const QString &username, quint32 token, const QString &query, governor::Permit search)
{
    if (!isLoggedIn() || username.isEmpty() || query.isEmpty() || !consumePermit(search, governor::Operation::Search)) {
        return false;
    }
    return send(toserver::UserSearch{username, token, query});
}

bool ServerSession::wishlistSearch(quint32 token, const QString &query, governor::Permit search)
{
    if (!isLoggedIn() || query.isEmpty() || !consumePermit(search, governor::Operation::Search)) {
        return false;
    }
    return send(toserver::WishlistSearch{token, query});
}

bool ServerSession::sendPrivateMessage(const QString &username, const QString &text, governor::Permit privateMessage)
{
    QString line = text;
    if (!isLoggedIn() || username.isEmpty() || prepareChatLine(line) != ChatTextError::None ||
        !consumePermit(privateMessage, governor::Operation::PrivateMessage)) {
        return false;
    }
    return send(toserver::MessageUser{username, line});
}

bool ServerSession::sayInRoom(const QString &room, const QString &text, governor::Permit roomMessage)
{
    QString line = text;
    if (!isLoggedIn() || m_rooms.value(room) != RoomState::Joined || prepareChatLine(line) != ChatTextError::None ||
        !consumePermit(roomMessage, governor::Operation::RoomMessage)) {
        return false;
    }
    return send(toserver::SayChatroom{room, line});
}

bool ServerSession::getPeerAddress(const QString &username, governor::Permit peerLookup)
{
    if (!isLoggedIn() || username.isEmpty() || !consumePermit(peerLookup, governor::Operation::PeerLookup)) {
        return false;
    }
    m_pendingAddressLookups.insert(username, QDeadlineTimer(protocol::kPeerConnectTimeout));
    return send(toserver::GetPeerAddress{username});
}

bool ServerSession::requestPeerConnection(quint32 token, const QString &username, ConnectionType type,
                                          governor::Permit peerLookup)
{
    if (!isLoggedIn() || username.isEmpty() || token == 0 ||
        !consumePermit(peerLookup, governor::Operation::PeerLookup)) {
        return false;
    }
    bool ok = send(toserver::ConnectToPeer{token, username, type});
    // One address lookup serves every attempt to that user that starts while it's outstanding.
    const auto pending = m_pendingAddressLookups.constFind(username);
    if (pending == m_pendingAddressLookups.constEnd() || pending->hasExpired()) {
        m_pendingAddressLookups.insert(username, QDeadlineTimer(protocol::kPeerConnectTimeout));
        ok = send(toserver::GetPeerAddress{username}) && ok;
    }
    return ok;
}

bool ServerSession::requestIndirectConnection(quint32 token, const QString &username, ConnectionType type,
                                              governor::Permit peerLookup)
{
    if (!isLoggedIn() || username.isEmpty() || token == 0 ||
        !consumePermit(peerLookup, governor::Operation::PeerLookup)) {
        return false;
    }
    return send(toserver::ConnectToPeer{token, username, type});
}

// --- unbudgeted sends ------------------------------------------------------------------------------------------------

bool ServerSession::watchUser(const QString &username)
{
    return !username.isEmpty() && sendIfLoggedIn(toserver::WatchUser{username});
}

bool ServerSession::unwatchUser(const QString &username)
{
    return !username.isEmpty() && sendIfLoggedIn(toserver::UnwatchUser{username});
}

bool ServerSession::requestUserStatus(const QString &username)
{
    return !username.isEmpty() && sendIfLoggedIn(toserver::GetUserStatus{username});
}

bool ServerSession::requestUserStats(const QString &username)
{
    return !username.isEmpty() && sendIfLoggedIn(toserver::GetUserStats{username});
}

bool ServerSession::checkPrivileges()
{
    return sendIfLoggedIn(toserver::CheckPrivileges{});
}

bool ServerSession::sendUploadSpeed(quint32 bytesPerSecond)
{
    return sendIfLoggedIn(toserver::SendUploadSpeed{bytesPerSecond});
}

bool ServerSession::requestRoomList()
{
    return sendIfLoggedIn(toserver::RoomList{});
}

bool ServerSession::joinRoom(const QString &room)
{
    if (!isLoggedIn() || validateRoomName(room) != ChatTextError::None) {
        return false;
    }
    const RoomState current = m_rooms.value(room, RoomState::Leaving);
    if (m_rooms.contains(room) && current != RoomState::Leaving) {
        return true; // already joined or joining
    }
    m_rooms.insert(room, RoomState::Joining);
    return send(toserver::JoinRoom{room, 0});
}

bool ServerSession::leaveRoom(const QString &room)
{
    const auto it = m_rooms.constFind(room);
    if (!isLoggedIn() || it == m_rooms.constEnd() || it.value() == RoomState::Leaving) {
        return false;
    }
    m_rooms.insert(room, RoomState::Leaving);
    return send(toserver::LeaveRoom{room});
}

QStringList ServerSession::joinedRooms() const
{
    QStringList rooms;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it.value() == RoomState::Joined) {
            rooms.append(it.key());
        }
    }
    rooms.sort();
    return rooms;
}

bool ServerSession::cantConnectToPeer(quint32 token, const QString &username)
{
    return !username.isEmpty() && sendIfLoggedIn(toserver::CantConnectToPeer{token, username});
}

bool ServerSession::sendHaveNoParent(bool noParent)
{
    return sendIfLoggedIn(toserver::HaveNoParent{noParent});
}

bool ServerSession::sendBranchLevel(quint32 level)
{
    return sendIfLoggedIn(toserver::BranchLevel{level});
}

bool ServerSession::sendBranchRoot(const QString &root)
{
    return sendIfLoggedIn(toserver::BranchRoot{root});
}

bool ServerSession::sendAcceptChildren(bool accept)
{
    return sendIfLoggedIn(toserver::AcceptChildren{accept});
}

} // namespace selecta::slsk
