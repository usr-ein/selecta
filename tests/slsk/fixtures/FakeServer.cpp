// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures/FakeServer.h"

#include <QDateTime>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::slsk::testing {
namespace {

quint32 ipv4(const QHostAddress &address)
{
    bool ok = false;
    const quint32 value = address.toIPv4Address(&ok);
    return ok ? value : 0;
}

} // namespace

FakeServer::FakeServer(QObject *parent)
    : QObject(parent)
    , m_listener(new QTcpServer(this))
{
    connect(m_listener, &QTcpServer::newConnection, this, &FakeServer::onNewConnection);
}

FakeServer::~FakeServer()
{
    for (auto &[socket, client] : m_clients) {
        socket->disconnect(this);
        socket->abort();
    }
}

bool FakeServer::start(quint16 port)
{
    return m_listener->listen(QHostAddress::LocalHost, port);
}

QString FakeServer::errorString() const
{
    return m_listener->errorString();
}

void FakeServer::setAccount(const QString &username, const QString &password)
{
    m_account = std::make_pair(username, password);
}

QHostAddress FakeServer::address() const
{
    return QHostAddress::LocalHost;
}

quint16 FakeServer::port() const
{
    return m_listener->serverPort();
}

void FakeServer::setLoginBehaviour(LoginBehaviour behaviour, const QString &reason, const QString &detail)
{
    m_loginBehaviour = behaviour;
    m_rejectReason = reason;
    m_rejectDetail = detail;
}

void FakeServer::addVirtualUser(const QString &username, const QHostAddress &address, quint16 port, VirtualUser *user)
{
    m_virtualUsers.insert(username, VirtualEntry{address, port, user});
}

void FakeServer::removeVirtualUser(const QString &username)
{
    m_virtualUsers.remove(username);
}

void FakeServer::setUserStats(const QString &username, const UserStats &stats)
{
    m_stats.insert(username, stats);
    if (Client *client = findLoggedIn(username)) {
        client->stats = stats;
    }
}

void FakeServer::onNewConnection()
{
    while (QTcpSocket *socket = m_listener->nextPendingConnection()) {
        socket->setParent(this);
        if (m_loginBehaviour == LoginBehaviour::CloseOnConnect) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        Client client;
        client.socket = socket;
        m_clients.emplace(socket, std::move(client));
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
    }
}

void FakeServer::onReadyRead(QTcpSocket *socket)
{
    const auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }
    Client &client = it->second;
    client.reader.append(socket->readAll());
    while (m_clients.contains(socket)) {
        const std::optional<Frame> frame = client.reader.next();
        if (!frame) {
            break;
        }
        const Decoded<ToServerMessage> decoded = decodeToServer(frame->code, frame->payload);
        if (!client.loggedIn) {
            if (frame->code == static_cast<quint32>(ServerCode::Login) && decoded.ok()) {
                handleLogin(client, std::get<toserver::Login>(*decoded.message));
            } else {
                client.codesBeforeLogin.append(frame->code); // Soulfind would leave these unread
            }
            continue;
        }
        if (!decoded.ok()) {
            continue;
        }
        client.received.append(*decoded.message);
        const QString username = client.username;
        handle(client, *decoded.message);
        Q_EMIT messageReceived(username, *decoded.message);
    }
    const auto still = m_clients.find(socket);
    if (still != m_clients.end() && still->second.reader.hasError()) {
        dropClient(socket); // over Soulfind's 8 KiB limit
    }
}

void FakeServer::onDisconnected(QTcpSocket *socket)
{
    dropClient(socket);
}

void FakeServer::dropClient(QTcpSocket *socket)
{
    const auto it = m_clients.find(socket);
    if (it == m_clients.end()) {
        return;
    }
    const QString username = it->second.loggedIn ? it->second.username : QString();
    m_clients.erase(it);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    if (username.isEmpty()) {
        return;
    }
    for (auto room = m_rooms.begin(); room != m_rooms.end(); ++room) {
        if (room->removeAll(username) > 0) {
            for (const QString &member : std::as_const(room.value())) {
                if (Client *other = findLoggedIn(member)) {
                    writeMessage(*other, fromserver::RoomUserLeft{room.key(), username});
                }
            }
        }
    }
    Q_EMIT clientDisconnected(username);
}

FakeServer::Client *FakeServer::findLoggedIn(const QString &username)
{
    for (auto &[socket, client] : m_clients) {
        if (client.loggedIn && client.username == username) {
            return &client;
        }
    }
    return nullptr;
}

bool FakeServer::write(Client &client, const QByteArray &bytes)
{
    Q_EMIT frameSent(client.username, bytes);
    return client.socket->write(bytes) == bytes.size();
}

UserStats FakeServer::statsOf(const QString &username) const
{
    return m_stats.value(username);
}

void FakeServer::handleLogin(Client &client, const toserver::Login &login)
{
    switch (m_loginBehaviour) {
    case LoginBehaviour::Silent:
        return;
    case LoginBehaviour::CloseOnLogin:
        dropClient(client.socket);
        return;
    case LoginBehaviour::Reject: {
        fromserver::LoginReply reply;
        reply.reason = m_rejectReason;
        reply.detail = m_rejectDetail;
        writeMessage(client, reply);
        client.socket->disconnectFromHost();
        return;
    }
    case LoginBehaviour::CloseOnConnect:
    case LoginBehaviour::Accept:
        break;
    }
    // Soulfind's checks: non-zero versions and a name.
    if (login.username.isEmpty() || login.majorVersion == 0 || login.minorVersion == 0) {
        fromserver::LoginReply reply;
        reply.reason = login.username.isEmpty() ? u"INVALIDUSERNAME"_s : u"INVALIDVERSION"_s;
        reply.detail = login.username.isEmpty() ? u"Nick empty."_s : QString();
        writeMessage(client, reply);
        client.socket->disconnectFromHost();
        return;
    }
    if (m_account && (login.username != m_account->first || login.password != m_account->second)) {
        fromserver::LoginReply reply;
        reply.reason = login.username != m_account->first ? u"INVALIDUSERNAME"_s : u"INVALIDPASS"_s;
        writeMessage(client, reply);
        client.socket->disconnectFromHost();
        return;
    }
    // A second login with the same name pushes the first one out.
    if (Client *previous = findLoggedIn(login.username)) {
        writeMessage(*previous, fromserver::Relogged{});
        previous->socket->flush();
        dropClient(previous->socket);
    }
    client.loggedIn = true;
    client.username = login.username;
    client.stats = statsOf(login.username);
    fromserver::LoginReply reply;
    reply.success = true;
    reply.greeting = m_greeting;
    reply.ownIp = ipv4(client.socket->peerAddress());
    reply.passwordHash = md5Hex(login.password.toUtf8());
    writeMessage(client, reply);
    // The welcome, in the official server's order.
    writeMessage(client, fromserver::RoomList{m_welcome.rooms, {}, {}, {}});
    if (m_welcome.parentMinSpeed) {
        writeMessage(client, fromserver::ParentMinSpeed{*m_welcome.parentMinSpeed});
    }
    if (m_welcome.parentSpeedRatio) {
        writeMessage(client, fromserver::ParentSpeedRatio{*m_welcome.parentSpeedRatio});
    }
    writeMessage(client, fromserver::WishlistInterval{m_welcome.wishlistInterval});
    writeMessage(client, fromserver::PrivilegedUsers{m_welcome.privilegedUsers});
    writeMessage(client, fromserver::ExcludedSearchPhrases{m_welcome.excludedPhrases});
    for (const fromserver::PrivateMessage &message : std::as_const(m_welcome.queuedMessages)) {
        writeMessage(client, message);
    }
    Q_EMIT clientLoggedIn(client.username);
}

// One small handler per client message.
struct FakeServer::Handler
{
    FakeServer &server;
    Client &client;

    void operator()(const toserver::Login & /*m*/) const {}
    void operator()(const toserver::SetWaitPort &m) const { client.listenPort = m.port; }
    void operator()(const toserver::UnwatchUser & /*m*/) const {}
    void operator()(toserver::MessageAcked /*m*/) const {}
    void operator()(toserver::ServerPing /*m*/) const {}
    void operator()(toserver::SetStatus m) const { client.status = m.status; }
    void operator()(toserver::HaveNoParent m) const { client.haveNoParent = m.noParent; }
    void operator()(toserver::AcceptChildren m) const { client.acceptChildren = m.accept; }
    void operator()(toserver::BranchLevel m) const { client.branchLevel = m.level; }
    void operator()(const toserver::BranchRoot &m) const { client.branchRoot = m.root; }
    void operator()(const toserver::FileSearch &m) const { server.routeSearch(client, m.token, m.query); }
    void operator()(const toserver::WishlistSearch &m) const { server.routeSearch(client, m.token, m.query); }

    void operator()(const toserver::GetPeerAddress &m) const
    {
        fromserver::PeerAddress reply{m.username, 0, 0, std::nullopt, std::nullopt};
        if (const Client *target = server.findLoggedIn(m.username); target != nullptr && target->listenPort != 0) {
            reply.ip = ipv4(target->socket->peerAddress());
            reply.port = target->listenPort;
        } else if (const auto v = server.m_virtualUsers.constFind(m.username); v != server.m_virtualUsers.constEnd()) {
            reply.ip = ipv4(v->address);
            reply.port = v->port;
        }
        server.writeMessage(client, reply);
    }

    bool isOnline(const QString &username) const
    {
        return server.findLoggedIn(username) != nullptr || server.m_virtualUsers.contains(username);
    }

    UserStats statsOf(const QString &username) const
    {
        const Client *target = server.findLoggedIn(username);
        return target != nullptr ? target->stats : server.statsOf(username);
    }

    void operator()(const toserver::WatchUser &m) const
    {
        fromserver::WatchUserReply reply;
        reply.username = m.username;
        reply.exists = isOnline(m.username) || server.m_stats.contains(m.username) || m.username == client.username;
        reply.status = isOnline(m.username) ? UserStatus::Online : UserStatus::Offline;
        reply.stats = statsOf(m.username);
        server.writeMessage(client, reply);
    }

    void operator()(const toserver::GetUserStatus &m) const
    {
        server.writeMessage(client,
                            fromserver::UserStatusUpdate{
                                m.username, isOnline(m.username) ? UserStatus::Online : UserStatus::Offline, false});
    }

    void operator()(const toserver::GetUserStats &m) const
    {
        server.writeMessage(client, fromserver::UserStatsUpdate{m.username, statsOf(m.username)});
    }

    void operator()(toserver::SharedFoldersFiles m) const
    {
        client.stats.files = m.files;
        client.stats.folders = m.folders;
    }

    void operator()(toserver::SendUploadSpeed m) const
    {
        // Soulfind's running mean.
        client.stats.averageSpeed = static_cast<quint32>((0.8 * client.stats.averageSpeed) + (0.2 * m.bytesPerSecond));
        ++client.stats.uploadCount;
    }

    void operator()(const toserver::SayChatroom &m) const
    {
        const QStringList members = server.m_rooms.value(m.room);
        if (!members.contains(client.username)) {
            return;
        }
        for (const QString &member : members) {
            if (Client *other = server.findLoggedIn(member)) {
                server.writeMessage(*other, fromserver::RoomMessage{m.room, client.username, m.message}); // echoed
            }
        }
    }

    void operator()(const toserver::JoinRoom &m) const
    {
        QStringList &members = server.m_rooms[m.room];
        if (members.contains(client.username)) {
            return;
        }
        const RoomMember me{client.username, UserStatus::Online, client.stats, 0, {}};
        // Like Soulfind and the official server: the joiner's own UserJoinedRoom comes first.
        server.writeMessage(client, fromserver::RoomUserJoined{m.room, me});
        for (const QString &member : std::as_const(members)) {
            if (Client *other = server.findLoggedIn(member)) {
                server.writeMessage(*other, fromserver::RoomUserJoined{m.room, me});
            }
        }
        members.append(client.username);
        fromserver::RoomJoined reply;
        reply.room = m.room;
        for (const QString &member : std::as_const(members)) {
            reply.members.append(RoomMember{member, UserStatus::Online, statsOf(member), 0, {}});
        }
        server.writeMessage(client, reply);
    }

    void operator()(const toserver::LeaveRoom &m) const
    {
        QStringList &members = server.m_rooms[m.room];
        if (members.removeAll(client.username) == 0) {
            return;
        }
        server.writeMessage(client, fromserver::RoomLeft{m.room});
        for (const QString &member : std::as_const(members)) {
            if (Client *other = server.findLoggedIn(member)) {
                server.writeMessage(*other, fromserver::RoomUserLeft{m.room, client.username});
            }
        }
    }

    void operator()(toserver::RoomList /*m*/) const
    {
        QList<RoomListEntry> rooms = server.m_welcome.rooms;
        for (auto it = server.m_rooms.constBegin(); it != server.m_rooms.constEnd(); ++it) {
            if (!it->isEmpty()) {
                rooms.append(RoomListEntry{it.key(), static_cast<quint32>(it->size())});
            }
        }
        server.writeMessage(client, fromserver::RoomList{rooms, {}, {}, {}});
    }

    void operator()(const toserver::ConnectToPeer &m) const
    {
        const fromserver::ConnectToPeerRequest request{
            client.username, m.type,      ipv4(client.socket->peerAddress()), client.listenPort, m.token, false,
            std::nullopt,    std::nullopt};
        if (Client *target = server.findLoggedIn(m.username)) {
            server.writeMessage(*target, request);
        } else if (const auto v = server.m_virtualUsers.constFind(m.username); v != server.m_virtualUsers.constEnd()) {
            v->user->connectToPeerRequested(request);
        } else {
            server.writeMessage(client, fromserver::CantConnectToPeer{m.token}); // offline: refused at once
        }
    }

    void operator()(const toserver::CantConnectToPeer &m) const
    {
        if (Client *target = server.findLoggedIn(m.username)) {
            server.writeMessage(*target, fromserver::CantConnectToPeer{m.token});
        } else if (const auto v = server.m_virtualUsers.constFind(m.username); v != server.m_virtualUsers.constEnd()) {
            v->user->cantConnectToPeer(m.token);
        }
    }

    void operator()(const toserver::MessageUser &m) const
    {
        server.sendPrivateMessage(client.username, m.username, m.message);
    }

    void operator()(const toserver::UserSearch &m) const
    {
        server.deliverSearch(m.username, fromserver::SearchRequest{client.username, m.token, m.query});
    }

    void operator()(toserver::CheckPrivileges /*m*/) const
    {
        server.writeMessage(client, fromserver::PrivilegesLeft{0});
    }

    void operator()(toserver::EnableRoomInvitations m) const
    {
        server.writeMessage(client, fromserver::RoomInvitationsEnabled{m.enable});
    }
};

void FakeServer::handle(Client &client, const ToServerMessage &message)
{
    std::visit(Handler{*this, client}, message);
}

void FakeServer::routeSearch(const Client &from, quint32 token, const QString &query)
{
    if (m_routing == SearchRouting::None) {
        return;
    }
    const fromserver::SearchRequest request{from.username, token, query};
    const QStringList users = loggedInUsers();
    for (const QString &user : users) {
        if (user != from.username || m_routing == SearchRouting::DirectIncludingSearcher) {
            deliverSearch(user, request);
        }
    }
    for (auto it = m_virtualUsers.constBegin(); it != m_virtualUsers.constEnd(); ++it) {
        it->user->searchRequested(request);
    }
}

void FakeServer::deliverSearch(const QString &to, const fromserver::SearchRequest &request)
{
    if (Client *target = findLoggedIn(to)) {
        writeMessage(*target, request);
    } else if (const auto v = m_virtualUsers.constFind(to); v != m_virtualUsers.constEnd()) {
        v->user->searchRequested(request);
    }
}

bool FakeServer::sendTo(const QString &username, const FromServerMessage &message)
{
    Client *const client = findLoggedIn(username);
    if (client == nullptr) {
        return false;
    }
    return std::visit([this, client](const auto &m) { return writeMessage(*client, m); }, message);
}

bool FakeServer::sendRawTo(const QString &username, const QByteArray &bytes)
{
    Client *client = findLoggedIn(username);
    return client != nullptr && write(*client, bytes);
}

void FakeServer::disconnectClient(const QString &username)
{
    if (const Client *client = findLoggedIn(username)) {
        dropClient(client->socket);
    }
}

bool FakeServer::relayConnectToPeer(const QString &fromVirtual, const QString &toClient, ConnectionType type,
                                    quint32 token)
{
    const auto v = m_virtualUsers.constFind(fromVirtual);
    Client *target = findLoggedIn(toClient);
    if (v == m_virtualUsers.constEnd() || target == nullptr) {
        return false;
    }
    return writeMessage(*target, fromserver::ConnectToPeerRequest{fromVirtual, type, ipv4(v->address), v->port, token,
                                                                  false, std::nullopt, std::nullopt});
}

bool FakeServer::relayCantConnectToPeer(quint32 token, const QString &toClient)
{
    Client *target = findLoggedIn(toClient);
    return target != nullptr && writeMessage(*target, fromserver::CantConnectToPeer{token});
}

bool FakeServer::sendPrivateMessage(const QString &from, const QString &toClient, const QString &text)
{
    Client *target = findLoggedIn(toClient);
    return target != nullptr &&
           writeMessage(*target, fromserver::PrivateMessage{m_nextMessageId++,
                                                            static_cast<quint32>(QDateTime::currentSecsSinceEpoch()),
                                                            from, text, true});
}

QStringList FakeServer::loggedInUsers() const
{
    QStringList users;
    for (const auto &[socket, client] : m_clients) {
        if (client.loggedIn) {
            users.append(client.username);
        }
    }
    users.sort();
    return users;
}

bool FakeServer::isLoggedIn(const QString &username) const
{
    return client(username) != nullptr;
}

const FakeServer::Client *FakeServer::client(const QString &username) const
{
    for (const auto &[socket, client] : m_clients) {
        if (client.loggedIn && client.username == username) {
            return &client;
        }
    }
    return nullptr;
}

QList<quint32> FakeServer::codesReceivedBeforeLogin() const
{
    QList<quint32> codes;
    for (const auto &[socket, client] : m_clients) {
        codes.append(client.codesBeforeLogin);
    }
    return codes;
}

QList<ToServerMessage> FakeServer::received(const QString &username) const
{
    const Client *c = client(username);
    return c != nullptr ? c->received : QList<ToServerMessage>();
}

} // namespace selecta::slsk::testing
