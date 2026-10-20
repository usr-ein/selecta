// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/PeerNetwork.h"

#include "slsk/Frame.h"
#include "slsk/Logging.h"
#include "slsk/MetaTypes.h"
#include "slsk/PeerMessages.h"
#include "slsk/Providers.h"
#include "slsk/ServerSession.h"
#include "slsk/TokenGenerator.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QMetaMethod>
#include <QNetworkInterface>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::slsk {

ShareProvider::~ShareProvider() = default;
UserInfoProvider::~UserInfoProvider() = default;
PermitSource::~PermitSource() = default;

const char *connectPurposeName(ConnectPurpose purpose)
{
    switch (purpose) {
    case ConnectPurpose::Request:
        return "request";
    case ConnectPurpose::SearchResponse:
        return "search response";
    case ConnectPurpose::Transfer:
        return "transfer";
    case ConnectPurpose::Distributed:
        return "distributed";
    }
    return "?";
}

const char *peerErrorName(PeerError error)
{
    switch (error) {
    case PeerError::None:
        return "none";
    case PeerError::NotLoggedIn:
        return "not logged in";
    case PeerError::NotPermitted:
        return "not permitted";
    case PeerError::UserOffline:
        return "user offline";
    case PeerError::Unreachable:
        return "unreachable";
    case PeerError::SocketLimit:
        return "socket limit reached";
    case PeerError::Timeout:
        return "timed out";
    case PeerError::ConnectionClosed:
        return "connection closed";
    case PeerError::ProtocolError:
        return "protocol error";
    case PeerError::Cancelled:
        return "cancelled";
    }
    return "?";
}

namespace internal {
namespace {

// Bytes a peer socket may hold before we read them (we drain it at once; this bounds a burst).
constexpr qint64 kSocketReadBuffer = qint64{256} * 1024;
// Answers aren't queued on a connection that already has this much unsent data: a peer that doesn't read can't make
// us buffer without bound.
constexpr qint64 kMaxUnsentAnswers = qint64{64} * 1024 * 1024;
// Payloads above this are inflated and parsed on a worker thread.
constexpr qsizetype kWorkerDecodeThreshold = protocol::kMiB;

struct PeerKey
{
    QString username;
    ConnectionType type = ConnectionType::Peer;

    friend bool operator==(const PeerKey &, const PeerKey &) = default;
};

size_t qHash(const PeerKey &key, size_t seed = 0) noexcept
{
    return qHashMulti(seed, key.username, static_cast<int>(key.type));
}

enum class Origin {
    Inbound,  // accepted by our listener
    Direct,   // we dialled and send PeerInit
    DialBack, // we dialled to answer a relayed ConnectToPeer and send PierceFireWall
};

struct Connection
{
    quint64 id = 0;
    QTcpSocket *socket = nullptr;
    Origin origin = Origin::Inbound;
    bool ready = false; // identified (inbound) or init sent (outbound)
    ConnectionType type = ConnectionType::Peer;
    QString username;
    FrameReader reader{CodeWidth::Byte, protocol::kPeerInitFrameMax};
    QDeadlineTimer deadline; // connect, init or idle, by phase
    quint64 attemptId = 0;   // Direct: the attempt it races for
    quint32 pierceToken = 0; // DialBack: the token to send
    bool openedByPeer = false;
    bool closeWhenFlushed = false;
    bool deliveredResults = false;
    QElapsedTimer progressClock; // share-list progress throttle
};

struct Pending
{
    QByteArray frame;
    PeerNetwork::SendCallback done;
    bool closeAfter = false; // a search answer: the connection may close once it's written
};

struct Attempt
{
    quint64 id = 0;
    QString username;
    ConnectionType type = ConnectionType::Peer;
    ConnectPurpose purpose = ConnectPurpose::Request;
    bool permitted = false; // the permit arrived and the race started
    quint32 token = 0;
    QDeadlineTimer deadline = QDeadlineTimer::Forever;
    bool directFailed = false;
    bool indirectFailed = false;
    quint64 directConnection = 0;
    std::optional<std::pair<QHostAddress, quint16>> knownAddress;
    quint64 fileRequest = 0;
    QList<Pending> queue;
};

struct PendingRequest
{
    QDeadlineTimer deadline = QDeadlineTimer::Forever; // set once the request is written
};

struct PendingFolder
{
    QString username;
    RemotePath folder;
    QDeadlineTimer deadline = QDeadlineTimer::Forever;
};

struct LateToken
{
    PeerKey key;
    QDeadlineTimer deadline;
};

using ShareListResult = std::optional<SharedFileList>;

} // namespace

class PeerNetworkPrivate
{
public:
    PeerNetworkPrivate(PeerNetwork *owner, ServerSession &session, PermitSource &permitSource)
        : q(owner)
        , server(session)
        , permits(permitSource)
        , listener(new QTcpServer(owner))
        , sweep(new QTimer(owner))
    {}
    ~PeerNetworkPrivate() = default;
    PeerNetworkPrivate(const PeerNetworkPrivate &) = delete;
    PeerNetworkPrivate &operator=(const PeerNetworkPrivate &) = delete;
    PeerNetworkPrivate(PeerNetworkPrivate &&) = delete;
    PeerNetworkPrivate &operator=(PeerNetworkPrivate &&) = delete;

    // --- bookkeeping -------------------------------------------------------------------------------------------------
    int socketCount() const { return static_cast<int>(connections.size() + handedOff.size()); }
    bool atSocketLimit() const { return socketCount() >= socketLimit; }
    Connection *find(quint64 id);
    Connection *readyPrimary(const PeerKey &key);
    quint64 addConnection(QTcpSocket *socket, Origin origin);
    void touch(Connection &c) const { c.deadline.setRemainingTime(timeouts.idle); }
    void setPrimary(const PeerKey &key, quint64 id);
    void close(quint64 id, PeerError reason, bool graceful = false);
    void onClosed(quint64 id);
    void failRequestsFor(const QString &username, PeerError error);
    void extendRequests(const QString &username);
    void restartSweep();
    void onSweep();

    // --- connection setup --------------------------------------------------------------------------------------------
    quint64 startAttempt(Attempt attempt);
    void onPermit(quint64 attemptId, std::optional<governor::Permit> permit);
    void onPeerAddress(const fromserver::PeerAddress &address);
    void onCantConnect(quint32 token);
    void onConnectToPeerRequested(const fromserver::ConnectToPeerRequest &request);
    void dialDirect(Attempt &attempt, const QHostAddress &address, quint16 port);
    QHostAddress reachable(const QHostAddress &address);
    void maybeFail(quint64 attemptId);
    std::optional<Attempt> takeAttempt(quint64 attemptId); // forgets it without a word
    void failAttempt(quint64 attemptId, PeerError error);
    void failLater(quint64 attemptId, PeerError error);
    void completeAttempt(quint64 attemptId, quint64 connectionId);
    void failAllAttempts(PeerError error);

    // --- socket events -----------------------------------------------------------------------------------------------
    void onNewConnection();
    void onConnected(quint64 id);
    void onSocketError(quint64 id);
    void onReadyRead(quint64 id);
    void onBytesWritten(quint64 id);
    void handleInit(quint64 id, const Frame &init);
    void becomeReady(Connection &c, ConnectionType type, const QString &username);
    void handOff(quint64 id, quint64 requestId);
    void processFrames(quint64 id);
    void handlePeerFrame(quint64 id, const Frame &f);
    void answerRequest(quint64 id, const Frame &f);
    void handleSearchResults(const QString &username, const QByteArray &payload) const;
    void handleShareList(const QString &username, const QByteArray &payload);
    void handleUserInfo(const QString &username, const QByteArray &payload);
    void handleFolderContents(const QString &username, const QByteArray &payload);
    void forwardTransferMessage(const QString &username, const Frame &f);
    void reportProgress(Connection &c) const;

    // --- sending -----------------------------------------------------------------------------------------------------
    void send(const QString &username, ConnectionType type, Pending pending, ConnectPurpose purpose);
    void write(Connection &c, const QByteArray &bytes) const;
    void answer(Connection &c, const QByteArray &bytes) const;

    PeerNetwork *q;
    ServerSession &server;
    PermitSource &permits;
    ShareProvider *shares = nullptr;
    UserInfoProvider *userInfo = nullptr;
    PeerNetwork::Timeouts timeouts;
    int socketLimit = protocol::kSocketsMax;
    QTcpServer *listener;
    QTimer *sweep;
    std::map<quint64, Connection> connections; // node-based: references survive other entries changing
    QHash<PeerKey, quint64> primary;
    std::map<quint64, Attempt> attempts;
    QHash<PeerKey, quint64> attemptByKey;   // P and D: at most one attempt each
    QHash<quint32, quint64> attemptByToken; // indirect tokens of attempts still racing
    QHash<quint32, LateToken> completedTokens;
    QSet<quint32> openSearches;
    QHash<QString, PendingRequest> pendingBrowse;
    QHash<QString, PendingRequest> pendingUserInfo;
    QHash<quint32, PendingFolder> pendingFolders;
    QSet<QObject *> handedOff;
    std::function<bool(const QString &)> fileFilter;
    std::function<bool(const QString &)> distributedFilter;
    TokenGenerator connectTokens;
    TokenGenerator folderTokens;
    quint64 nextId = 1;
    quint64 starting = 0;                // the attempt whose permit startAttempt() is asking for
    QList<QHostAddress> ownAddresses;    // this machine's interface addresses, for reachable()
    QDeadlineTimer ownAddressesFresh{0}; // when to read them again
};

// --- bookkeeping -----------------------------------------------------------------------------------------------------

Connection *PeerNetworkPrivate::find(quint64 id)
{
    const auto it = connections.find(id);
    return it == connections.end() ? nullptr : &it->second;
}

Connection *PeerNetworkPrivate::readyPrimary(const PeerKey &key)
{
    const auto it = primary.constFind(key);
    if (it == primary.constEnd()) {
        return nullptr;
    }
    Connection *c = find(it.value());
    return c != nullptr && c->ready ? c : nullptr;
}

quint64 PeerNetworkPrivate::addConnection(QTcpSocket *socket, Origin origin)
{
    const quint64 id = nextId++;
    socket->setParent(q);
    socket->setReadBufferSize(kSocketReadBuffer);
    Connection c;
    c.id = id;
    c.socket = socket;
    c.origin = origin;
    connections.emplace(id, std::move(c));
    QObject::connect(socket, &QTcpSocket::connected, q, [this, id] { onConnected(id); });
    QObject::connect(socket, &QTcpSocket::readyRead, q, [this, id] { onReadyRead(id); });
    QObject::connect(socket, &QTcpSocket::bytesWritten, q, [this, id] { onBytesWritten(id); });
    QObject::connect(socket, &QTcpSocket::errorOccurred, q, [this, id] { onSocketError(id); });
    QObject::connect(socket, &QTcpSocket::disconnected, q, [this, id] { onClosed(id); });
    return id;
}

void PeerNetworkPrivate::setPrimary(const PeerKey &key, quint64 id)
{
    // The newest connection is the one we send on; an older one stays open for reading until it closes or idles out,
    // so nothing the peer already sent on it is lost.
    primary.insert(key, id);
}

void PeerNetworkPrivate::close(quint64 id, PeerError reason, bool graceful)
{
    const Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    if (graceful && c->socket->state() == QAbstractSocket::ConnectedState) {
        // Flush what we wrote, then let the peer close; onClosed() cleans up.
        c->socket->disconnectFromHost();
        return;
    }
    Q_UNUSED(reason)
    onClosed(id);
}

void PeerNetworkPrivate::onClosed(quint64 id)
{
    const auto it = connections.find(id);
    if (it == connections.end()) {
        return;
    }
    const Connection c = std::move(it->second);
    connections.erase(it);
    c.socket->disconnect(q);
    c.socket->abort();
    c.socket->deleteLater();

    // An attempt's direct path just failed.
    if (c.origin == Origin::Direct && !c.ready && c.attemptId != 0) {
        const auto a = attempts.find(c.attemptId);
        if (a != attempts.end() && a->second.directConnection == id) {
            a->second.directConnection = 0;
            a->second.directFailed = true;
            maybeFail(c.attemptId);
        }
        return;
    }
    // Our dial-back failed before it could identify us: tell the server (protocol §3.2).
    if (c.origin == Origin::DialBack && !c.ready) {
        qCDebug(lcSlskPeer).noquote() << "dial-back to" << c.username << "failed";
        server.cantConnectToPeer(c.pierceToken, c.username);
        return;
    }
    if (!c.ready) {
        return; // an inbound socket that never identified itself
    }
    const PeerKey key{c.username, c.type};
    if (primary.value(key) == id) {
        primary.remove(key);
        // Promote another open connection of the same kind, if any.
        for (auto &[otherId, other] : connections) {
            if (other.ready && other.username == c.username && other.type == c.type) {
                primary.insert(key, otherId);
                break;
            }
        }
    }
    const bool stillConnected = primary.contains(key);
    if (c.type == ConnectionType::Peer && !stillConnected) {
        failRequestsFor(c.username, PeerError::ConnectionClosed);
    }
    if (c.type == ConnectionType::Distributed && !stillConnected) {
        Q_EMIT q->distributedDisconnected(c.username);
    }
}

void PeerNetworkPrivate::failRequestsFor(const QString &username, PeerError error)
{
    // Only requests already written can be answered on the connection that closed; queued ones wait on their attempt.
    if (const auto it = pendingBrowse.constFind(username);
        it != pendingBrowse.constEnd() && !it->deadline.isForever()) {
        pendingBrowse.erase(it);
        Q_EMIT q->requestFailed(username, PeerRequest::SharedFileList, 0, error);
    }
    if (const auto it = pendingUserInfo.constFind(username);
        it != pendingUserInfo.constEnd() && !it->deadline.isForever()) {
        pendingUserInfo.erase(it);
        Q_EMIT q->requestFailed(username, PeerRequest::UserInfo, 0, error);
    }
    QList<quint32> folders;
    for (auto it = pendingFolders.constBegin(); it != pendingFolders.constEnd(); ++it) {
        if (it->username == username && !it->deadline.isForever()) {
            folders.append(it.key());
        }
    }
    for (const quint32 token : std::as_const(folders)) {
        pendingFolders.remove(token);
        Q_EMIT q->requestFailed(username, PeerRequest::FolderContents, token, error);
    }
}

void PeerNetworkPrivate::extendRequests(const QString &username)
{
    // Traffic from the peer means it's working on the answer (a big share list can take a while).
    if (const auto it = pendingBrowse.find(username); it != pendingBrowse.end() && !it->deadline.isForever()) {
        it->deadline.setRemainingTime(timeouts.response);
    }
    if (const auto it = pendingUserInfo.find(username); it != pendingUserInfo.end() && !it->deadline.isForever()) {
        it->deadline.setRemainingTime(timeouts.response);
    }
    for (auto &folder : pendingFolders) {
        if (folder.username == username && !folder.deadline.isForever()) {
            folder.deadline.setRemainingTime(timeouts.response);
        }
    }
}

void PeerNetworkPrivate::restartSweep()
{
    const auto shortest =
        std::min({timeouts.connect, timeouts.indirect, timeouts.idle, timeouts.init, timeouts.response});
    sweep->start(std::clamp<std::chrono::milliseconds>(shortest / 4, 10ms, 1s));
}

void PeerNetworkPrivate::onSweep()
{
    // Attempts whose time ran out: the indirect path is over; a direct dial still in progress has its own deadline.
    QList<quint64> expiredAttempts;
    for (auto &[id, attempt] : attempts) {
        if (attempt.permitted && attempt.deadline.hasExpired()) {
            expiredAttempts.append(id);
        }
    }
    for (const quint64 id : std::as_const(expiredAttempts)) {
        const auto a = attempts.find(id);
        if (a == attempts.end()) {
            continue;
        }
        a->second.indirectFailed = true;
        if (a->second.directConnection == 0) {
            a->second.directFailed = true; // the address never came, or the direct path was impossible
        }
        maybeFail(id);
    }
    // Connections past their deadline: connect timeout, unidentified inbound socket, or idle.
    QList<quint64> expiredConnections;
    for (auto &[id, connection] : connections) {
        if (connection.deadline.hasExpired()) {
            expiredConnections.append(id);
        }
    }
    for (const quint64 id : std::as_const(expiredConnections)) {
        if (const Connection *c = find(id); c != nullptr && c->ready) {
            qCDebug(lcSlskPeer).noquote() << "closing idle connection to" << c->username;
        }
        close(id, PeerError::Timeout);
    }
    // Late PierceFireWall tokens are remembered only for the indirect window.
    for (auto it = completedTokens.begin(); it != completedTokens.end();) {
        it = it->deadline.hasExpired() ? completedTokens.erase(it) : std::next(it);
    }
    // Requests with no answer and no traffic.
    QStringList browses;
    for (auto it = pendingBrowse.constBegin(); it != pendingBrowse.constEnd(); ++it) {
        if (it->deadline.hasExpired()) {
            browses.append(it.key());
        }
    }
    for (const QString &user : std::as_const(browses)) {
        pendingBrowse.remove(user);
        Q_EMIT q->requestFailed(user, PeerRequest::SharedFileList, 0, PeerError::Timeout);
    }
    QStringList infos;
    for (auto it = pendingUserInfo.constBegin(); it != pendingUserInfo.constEnd(); ++it) {
        if (it->deadline.hasExpired()) {
            infos.append(it.key());
        }
    }
    for (const QString &user : std::as_const(infos)) {
        pendingUserInfo.remove(user);
        Q_EMIT q->requestFailed(user, PeerRequest::UserInfo, 0, PeerError::Timeout);
    }
    QList<quint32> folders;
    for (auto it = pendingFolders.constBegin(); it != pendingFolders.constEnd(); ++it) {
        if (it->deadline.hasExpired()) {
            folders.append(it.key());
        }
    }
    for (const quint32 token : std::as_const(folders)) {
        const QString user = pendingFolders.take(token).username;
        Q_EMIT q->requestFailed(user, PeerRequest::FolderContents, token, PeerError::Timeout);
    }
}

// --- connection setup ------------------------------------------------------------------------------------------------

quint64 PeerNetworkPrivate::startAttempt(Attempt attempt)
{
    // Nothing is reported from inside this call. An attempt that fails while it starts (no session, no socket to
    // spare, a permit refused on the spot) fails on the next event-loop turn instead, so the caller holds whatever id
    // it was given before hearing of it, and cancelling it meanwhile (cancelFileConnection()) silences it.
    const quint64 id = nextId++;
    attempt.id = id;
    const QString username = attempt.username;
    const ConnectPurpose purpose = attempt.purpose;
    if (attempt.type != ConnectionType::File) {
        attemptByKey.insert(PeerKey{attempt.username, attempt.type}, id);
    }
    attempts.emplace(id, std::move(attempt));
    if (!server.isLoggedIn()) {
        failLater(id, PeerError::NotLoggedIn);
        return id;
    }
    if (atSocketLimit()) {
        failLater(id, PeerError::SocketLimit);
        return id;
    }
    qCDebug(lcSlskPeer).noquote() << "connecting to" << username << "for" << connectPurposeName(purpose);
    // The callback may run now (tests) or on a later turn (core/); it holds no raw pointer.
    const QPointer<PeerNetwork> guard(q);
    starting = id;
    permits.requestPeerLookup(username, purpose, [guard, id](std::optional<governor::Permit> permit) {
        if (guard) {
            guard->d->onPermit(id, std::move(permit));
        }
    });
    starting = 0;
    return id;
}

void PeerNetworkPrivate::failLater(quint64 attemptId, PeerError error)
{
    QTimer::singleShot(0, q, [this, attemptId, error] { failAttempt(attemptId, error); });
}

void PeerNetworkPrivate::onPermit(quint64 attemptId, std::optional<governor::Permit> permit)
{
    const auto it = attempts.find(attemptId);
    if (it == attempts.end() || it->second.permitted) {
        return; // given up meanwhile, or a duplicate callback: the permit is dropped
    }
    if (!permit) {
        failAttempt(attemptId, PeerError::NotPermitted);
        return;
    }
    Attempt &a = it->second;
    const quint32 token =
        connectTokens.next([this](quint32 t) { return attemptByToken.contains(t) || completedTokens.contains(t); });
    a.token = token;
    a.permitted = true;
    a.deadline.setRemainingTime(std::max(timeouts.connect, timeouts.indirect));
    attemptByToken.insert(token, attemptId);
    const bool sent = a.knownAddress ? server.requestIndirectConnection(token, a.username, a.type, std::move(*permit))
                                     : server.requestPeerConnection(token, a.username, a.type, std::move(*permit));
    if (!sent) {
        failAttempt(attemptId, PeerError::NotLoggedIn);
        return;
    }
    const auto again = attempts.find(attemptId);
    if (again == attempts.end()) {
        return;
    }
    if (const std::optional<std::pair<QHostAddress, quint16>> known = again->second.knownAddress) {
        dialDirect(again->second, known->first, known->second);
    }
}

void PeerNetworkPrivate::onPeerAddress(const fromserver::PeerAddress &address)
{
    QList<quint64> waiting;
    for (auto &[id, attempt] : attempts) {
        if (attempt.username == address.username && attempt.permitted && !attempt.knownAddress &&
            attempt.directConnection == 0 && !attempt.directFailed) {
            waiting.append(id);
        }
    }
    for (const quint64 id : std::as_const(waiting)) {
        const auto it = attempts.find(id);
        if (it == attempts.end()) {
            continue;
        }
        if (address.ip == 0) {
            failAttempt(id, PeerError::UserOffline); // 0.0.0.0: not connected to the server
            continue;
        }
        if (address.port == 0 || address.port > 65535) {
            it->second.directFailed = true; // only the indirect path can work
            maybeFail(id);
            continue;
        }
        dialDirect(it->second, ipv4Address(address.ip), static_cast<quint16>(address.port));
    }
}

void PeerNetworkPrivate::dialDirect(Attempt &attempt, const QHostAddress &address, quint16 port)
{
    if (atSocketLimit()) {
        attempt.directFailed = true;
        maybeFail(attempt.id);
        return;
    }
    const quint64 attemptId = attempt.id;
    auto *socket = new QTcpSocket(q);
    const quint64 id = addConnection(socket, Origin::Direct);
    Connection &c = connections.at(id);
    c.type = attempt.type;
    c.username = attempt.username;
    c.attemptId = attemptId;
    c.deadline.setRemainingTime(timeouts.connect);
    attempt.directConnection = id;
    socket->connectToHost(reachable(address), port);
}

QHostAddress PeerNetworkPrivate::reachable(const QHostAddress &address)
{
    // A peer at one of this machine's own addresses is on this machine: reach it over loopback. A local server
    // (Soulfind) hands out the LAN address for clients that log in from 127.0.0.1, and the host's firewall may drop
    // connections to that address from programs it doesn't know.
    if (ownAddressesFresh.hasExpired()) {
        ownAddresses = QNetworkInterface::allAddresses();
        ownAddressesFresh.setRemainingTime(std::chrono::minutes(1));
    }
    const bool own =
        !address.isLoopback() && address.protocol() == QAbstractSocket::IPv4Protocol && ownAddresses.contains(address);
    return own ? QHostAddress(QHostAddress::LocalHost) : address;
}

void PeerNetworkPrivate::maybeFail(quint64 attemptId)
{
    const auto it = attempts.find(attemptId);
    if (it != attempts.end() && it->second.directFailed && it->second.indirectFailed) {
        failAttempt(attemptId, PeerError::Unreachable);
    }
}

void PeerNetworkPrivate::onCantConnect(quint32 token)
{
    const auto it = attemptByToken.constFind(token);
    if (it == attemptByToken.constEnd()) {
        return;
    }
    const quint64 attemptId = it.value();
    const auto a = attempts.find(attemptId);
    if (a == attempts.end()) {
        return;
    }
    a->second.indirectFailed = true;
    maybeFail(attemptId);
}

std::optional<Attempt> PeerNetworkPrivate::takeAttempt(quint64 attemptId)
{
    const auto it = attempts.find(attemptId);
    if (it == attempts.end()) {
        return std::nullopt;
    }
    Attempt a = std::move(it->second);
    attempts.erase(it);
    if (a.type != ConnectionType::File) {
        const PeerKey key{a.username, a.type};
        if (attemptByKey.value(key) == attemptId) {
            attemptByKey.remove(key);
        }
    }
    if (a.token != 0) {
        attemptByToken.remove(a.token);
    }
    if (a.directConnection != 0) {
        if (Connection *c = find(a.directConnection)) {
            c->attemptId = 0; // closing it must not report back to the attempt we're dropping
            onClosed(a.directConnection);
        }
    }
    return a;
}

void PeerNetworkPrivate::failAttempt(quint64 attemptId, PeerError error)
{
    if (attemptId == starting) {
        failLater(attemptId, error); // a permit refused inside startAttempt(): see there
        return;
    }
    const std::optional<Attempt> taken = takeAttempt(attemptId);
    if (!taken) {
        return;
    }
    const Attempt &a = *taken;
    qCDebug(lcSlskPeer).noquote() << "could not connect to" << a.username << ":" << peerErrorName(error);
    for (const Pending &pending : a.queue) {
        if (pending.done) {
            pending.done(error);
        }
    }
    if (a.type == ConnectionType::File) {
        Q_EMIT q->fileConnectionFailed(a.fileRequest, a.username, error);
    } else if (a.type == ConnectionType::Distributed) {
        Q_EMIT q->distributedConnectFailed(a.username, error);
    }
}

void PeerNetworkPrivate::completeAttempt(quint64 attemptId, quint64 connectionId)
{
    const auto it = attempts.find(attemptId);
    if (it == attempts.end()) {
        return;
    }
    Attempt a = std::move(it->second);
    attempts.erase(it);
    const PeerKey key{a.username, a.type};
    if (a.type != ConnectionType::File && attemptByKey.value(key) == attemptId) {
        attemptByKey.remove(key);
    }
    if (a.token != 0) {
        attemptByToken.remove(a.token);
        completedTokens.insert(a.token, LateToken{key, QDeadlineTimer(timeouts.indirect)});
    }
    // The race is over: drop the other path's socket if it's still dialling.
    if (a.directConnection != 0 && a.directConnection != connectionId) {
        if (Connection *loser = find(a.directConnection)) {
            loser->attemptId = 0;
            onClosed(a.directConnection);
        }
    }
    Connection *c = find(connectionId);
    if (c == nullptr) {
        for (const Pending &pending : std::as_const(a.queue)) {
            if (pending.done) {
                pending.done(PeerError::ConnectionClosed);
            }
        }
        return;
    }
    if (a.type == ConnectionType::File) {
        handOff(connectionId, a.fileRequest);
        return;
    }
    setPrimary(key, connectionId);
    const bool onlySearchAnswers =
        !a.queue.isEmpty() && std::ranges::all_of(a.queue, [](const Pending &p) { return p.closeAfter; });
    c->closeWhenFlushed = onlySearchAnswers && a.purpose == ConnectPurpose::SearchResponse;
    for (const Pending &pending : std::as_const(a.queue)) {
        if (Connection *live = find(connectionId)) {
            write(*live, pending.frame);
            if (pending.done) {
                pending.done(PeerError::None);
            }
        } else if (pending.done) {
            pending.done(PeerError::ConnectionClosed);
        }
    }
    if (a.type == ConnectionType::Distributed) {
        Q_EMIT q->distributedConnected(a.username, true);
    }
}

void PeerNetworkPrivate::failAllAttempts(PeerError error)
{
    QList<quint64> ids;
    for (const auto &[id, attempt] : attempts) {
        ids.append(id);
    }
    for (const quint64 id : std::as_const(ids)) {
        failAttempt(id, error);
    }
}

void PeerNetworkPrivate::onConnectToPeerRequested(const fromserver::ConnectToPeerRequest &request)
{
    bool wanted = true;
    if (request.type == ConnectionType::File) {
        wanted = fileFilter && fileFilter(request.username);
    } else if (request.type == ConnectionType::Distributed) {
        wanted = distributedFilter && distributedFilter(request.username);
    }
    const bool addressValid = request.ip != 0 && request.port != 0 && request.port <= 65535;
    if (!wanted || !addressValid || atSocketLimit() || request.username.isEmpty()) {
        qCDebug(lcSlskPeer).noquote() << "not answering indirect request from" << request.username << "type"
                                      << connectionTypeCode(request.type);
        server.cantConnectToPeer(request.token, request.username);
        return;
    }
    auto *socket = new QTcpSocket(q);
    const quint64 id = addConnection(socket, Origin::DialBack);
    Connection &c = connections.at(id);
    c.type = request.type;
    c.username = request.username;
    c.pierceToken = request.token;
    c.openedByPeer = true; // the peer asked for this connection
    c.deadline.setRemainingTime(timeouts.connect);
    socket->connectToHost(reachable(ipv4Address(request.ip)), static_cast<quint16>(request.port));
}

// --- socket events ---------------------------------------------------------------------------------------------------

void PeerNetworkPrivate::onNewConnection()
{
    while (QTcpSocket *socket = listener->nextPendingConnection()) {
        if (atSocketLimit()) {
            qCWarning(lcSlskPeer) << "socket limit reached; refusing an incoming connection";
            socket->abort();
            socket->deleteLater();
            continue;
        }
        const quint64 id = addConnection(socket, Origin::Inbound);
        connections.at(id).deadline.setRemainingTime(timeouts.init);
        // Bytes may already be waiting.
        if (socket->bytesAvailable() > 0) {
            onReadyRead(id);
        }
    }
}

void PeerNetworkPrivate::onConnected(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    c->socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    if (c->origin == Origin::Direct) {
        const QString self = server.username();
        if (self.isEmpty()) {
            close(id, PeerError::NotLoggedIn);
            return;
        }
        c->socket->write(frame(peerinit::PeerInit{self, c->type, 0}));
        const quint64 attemptId = c->attemptId;
        becomeReady(*c, c->type, c->username);
        completeAttempt(attemptId, id);
        return;
    }
    if (c->origin == Origin::DialBack) {
        c->socket->write(frame(peerinit::PierceFireWall{c->pierceToken}));
        const ConnectionType type = c->type;
        const QString username = c->username;
        becomeReady(*c, type, username);
        switch (type) {
        case ConnectionType::Peer: {
            const PeerKey key{username, type};
            if (const auto attempt = attemptByKey.constFind(key); attempt != attemptByKey.constEnd()) {
                completeAttempt(attempt.value(), id); // it serves our own pending attempt too
            } else {
                setPrimary(key, id);
            }
            break;
        }
        case ConnectionType::File:
            handOff(id, 0);
            break;
        case ConnectionType::Distributed:
            setPrimary(PeerKey{username, type}, id);
            Q_EMIT q->distributedConnected(username, false);
            break;
        }
    }
}

void PeerNetworkPrivate::onSocketError(quint64 id)
{
    if (const Connection *c = find(id)) {
        qCDebug(lcSlskPeer).noquote() << "connection to" << (c->username.isEmpty() ? u"?"_s : c->username)
                                      << "error:" << c->socket->errorString();
        onClosed(id);
    }
}

void PeerNetworkPrivate::becomeReady(Connection &c, ConnectionType type, const QString &username)
{
    c.ready = true;
    c.type = type;
    c.username = username;
    touch(c);
    const QByteArray rest = c.reader.takeBuffered();
    if (type == ConnectionType::Peer) {
        c.reader = FrameReader(CodeWidth::Word, protocol::kPeerFrameMax);
        // Big answers only while we wait for one from this user; unknown codes get Nicotine+'s 16 MiB.
        c.reader.setCapFunction([this, username](quint32 code) -> qsizetype {
            switch (static_cast<PeerCode>(code)) {
            case PeerCode::SharedFileListResponse:
                return pendingBrowse.contains(username) ? protocol::kShareListFrameMax : -1;
            case PeerCode::UserInfoResponse:
                return pendingUserInfo.contains(username) ? protocol::kUserInfoFrameMax : -1;
            case PeerCode::FileSearchResponse:
            case PeerCode::FolderContentsResponse:
                return protocol::kPeerLargeFrameMax;
            default:
                return isKnownPeerCode(code) ? protocol::kPeerFrameMax : protocol::kPeerLargeFrameMax;
            }
        });
    } else if (type == ConnectionType::Distributed) {
        c.reader = FrameReader(CodeWidth::Byte, protocol::kDistribFrameMax);
    }
    c.reader.append(rest); // an F connection keeps these bytes for the hand-off
}

void PeerNetworkPrivate::handOff(quint64 id, quint64 requestId)
{
    const auto it = connections.find(id);
    if (it == connections.end()) {
        return;
    }
    Connection c = std::move(it->second);
    connections.erase(it);
    c.socket->disconnect(q);
    const QByteArray pending = c.reader.takeBuffered();
    if (!q->isSignalConnected(QMetaMethod::fromSignal(&PeerNetwork::fileConnectionReady))) {
        qCWarning(lcSlskPeer) << "no receiver for a file connection; closing it";
        c.socket->abort();
        c.socket->deleteLater();
        return;
    }
    QTcpSocket *socket = c.socket;
    handedOff.insert(socket);
    QObject::connect(socket, &QObject::destroyed, q, [this, socket] { handedOff.remove(socket); });
    Q_EMIT q->fileConnectionReady(requestId, c.username, socket, pending);
}

void PeerNetworkPrivate::onReadyRead(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    if (c->ready) {
        touch(*c);
    }
    c->reader.append(c->socket->readAll());
    if (!c->ready) {
        if (c->origin != Origin::Inbound) {
            return; // outbound sockets send first; nothing is expected before we identify ourselves
        }
        const std::optional<Frame> init = c->reader.next();
        if (!init) {
            if (c->reader.hasError()) {
                qCDebug(lcSlskPeer) << "bad peer-init frame:" << frameErrorName(c->reader.error());
                close(id, PeerError::ProtocolError);
            }
            return;
        }
        handleInit(id, *init);
        c = find(id);
        if (c == nullptr || !c->ready || c->type == ConnectionType::File) {
            return;
        }
    }
    processFrames(id);
}

void PeerNetworkPrivate::handleInit(quint64 id, const Frame &init)
{
    const Decoded<PeerInitMessage> decoded = decodePeerInit(init.code, init.payload);
    if (!decoded.ok() || !decoded.message) {
        qCDebug(lcSlskPeer) << "unusable peer-init message, code" << init.code;
        close(id, PeerError::ProtocolError);
        return;
    }
    Connection &c = connections.at(id);
    if (const auto *pierce = std::get_if<peerinit::PierceFireWall>(&*decoded.message)) {
        if (const auto it = attemptByToken.constFind(pierce->token); it != attemptByToken.constEnd()) {
            const quint64 attemptId = it.value();
            const Attempt &a = attempts.at(attemptId);
            becomeReady(c, a.type, a.username);
            completeAttempt(attemptId, id);
            return;
        }
        if (const auto late = completedTokens.constFind(pierce->token); late != completedTokens.constEnd()) {
            // Our direct connection won the race; the peer dialled back anyway.
            if (late->key.type == ConnectionType::File) {
                close(id, PeerError::Cancelled);
                return;
            }
            becomeReady(c, late->key.type, late->key.username); // kept for reading; not the one we send on
            return;
        }
        qCDebug(lcSlskPeer) << "PierceFireWall with an unknown token" << pierce->token;
        close(id, PeerError::ProtocolError);
        return;
    }
    const auto &peerInit = std::get<peerinit::PeerInit>(*decoded.message);
    if (!isAcceptablePeerUsername(peerInit.username)) {
        qCDebug(lcSlskPeer) << "refused a peer with an unacceptable username";
        close(id, PeerError::ProtocolError);
        return;
    }
    c.openedByPeer = true;
    becomeReady(c, peerInit.type, peerInit.username);
    const PeerKey key{peerInit.username, peerInit.type};
    switch (peerInit.type) {
    case ConnectionType::Peer:
        if (const auto attempt = attemptByKey.constFind(key); attempt != attemptByKey.constEnd()) {
            completeAttempt(attempt.value(), id); // the peer reached us first: our queued messages go here
        } else {
            setPrimary(key, id);
        }
        break;
    case ConnectionType::File:
        handOff(id, 0);
        break;
    case ConnectionType::Distributed:
        if (const auto attempt = attemptByKey.constFind(key); attempt != attemptByKey.constEnd()) {
            completeAttempt(attempt.value(), id);
        } else {
            setPrimary(key, id);
            Q_EMIT q->distributedConnected(peerInit.username, false);
        }
        break;
    }
}

void PeerNetworkPrivate::processFrames(quint64 id)
{
    for (;;) {
        Connection *c = find(id);
        if (c == nullptr) {
            return;
        }
        const std::optional<Frame> f = c->reader.next();
        if (!f) {
            if (c->reader.hasError()) {
                qCWarning(lcSlskPeer).noquote()
                    << "closing connection to" << c->username << ":" << frameErrorName(c->reader.error());
                close(id, PeerError::ProtocolError);
                return;
            }
            if (c->type == ConnectionType::Peer) {
                reportProgress(*c);
                // A connection the peer opened just to deliver search results is done once they're read.
                if (c->deliveredResults && c->openedByPeer && !pendingBrowse.contains(c->username) &&
                    !pendingUserInfo.contains(c->username) && c->socket->bytesToWrite() == 0) {
                    close(id, PeerError::None, true);
                }
            }
            return;
        }
        if (c->type == ConnectionType::Distributed) {
            Q_EMIT q->distributedMessageReceived(c->username, f->code, f->payload);
        } else {
            extendRequests(c->username);
            handlePeerFrame(id, *f);
        }
    }
}

void PeerNetworkPrivate::reportProgress(Connection &c) const
{
    if (c.reader.pendingFrameCode() != static_cast<quint32>(PeerCode::SharedFileListResponse)) {
        return;
    }
    if (c.progressClock.isValid() && c.progressClock.elapsed() < protocol::kProgressInterval.count()) {
        return;
    }
    c.progressClock.start();
    Q_EMIT q->sharedFileListProgress(c.username, c.reader.pendingFrameReceived(), c.reader.pendingFrameSize());
}

void PeerNetworkPrivate::handlePeerFrame(quint64 id, const Frame &f)
{
    const QString username = connections.at(id).username;
    switch (static_cast<PeerCode>(f.code)) {
    case PeerCode::GetShareFileList:
    case PeerCode::UserInfoRequest:
    case PeerCode::FolderContentsRequest:
        answerRequest(id, f);
        return;
    case PeerCode::FileSearchResponse:
        connections.at(id).deliveredResults = true;
        handleSearchResults(username, f.payload);
        return;
    case PeerCode::SharedFileListResponse:
        handleShareList(username, f.payload);
        return;
    case PeerCode::UserInfoResponse:
        handleUserInfo(username, f.payload);
        return;
    case PeerCode::FolderContentsResponse:
        handleFolderContents(username, f.payload);
        return;
    default:
        forwardTransferMessage(username, f);
        return;
    }
}

void PeerNetworkPrivate::answerRequest(quint64 id, const Frame &f)
{
    const QString username = connections.at(id).username;
    // Each answer goes back on the connection the question came on; the signal first, so core/ sees who asked.
    if (f.code == static_cast<quint32>(PeerCode::GetShareFileList)) {
        Q_EMIT q->sharedFileListRequested(username);
        const QByteArray payload =
            shares != nullptr ? shares->sharedFileList(username) : peer::SharedFileListResponse{}.encode();
        if (payload.size() > protocol::kShareListFrameMax) {
            qCWarning(lcSlskPeer) << "share list too large to send:" << payload.size() << "bytes";
            return;
        }
        if (Connection *live = find(id)) {
            answer(*live,
                   encodeFrame(CodeWidth::Word, static_cast<quint32>(PeerCode::SharedFileListResponse), payload));
        }
        return;
    }
    if (f.code == static_cast<quint32>(PeerCode::UserInfoRequest)) {
        Q_EMIT q->userInfoRequested(username);
        const peer::UserInfoResponse info =
            userInfo != nullptr ? userInfo->userInfo(username) : peer::UserInfoResponse{};
        if (Connection *live = find(id)) {
            answer(*live, frame(info));
        }
        return;
    }
    const std::optional<peer::FolderContentsRequest> request = peer::FolderContentsRequest::decode(f.payload);
    if (!request) {
        qCDebug(lcSlskPeer) << "dropped malformed FolderContentsRequest";
        return;
    }
    Q_EMIT q->folderContentsRequested(username, request->folder);
    peer::FolderContentsResponse response;
    response.token = request->token;
    response.folder = request->folder;
    if (shares != nullptr) {
        response.directories = shares->folderContents(username, request->folder);
    }
    if (Connection *live = find(id)) {
        answer(*live, frame(response));
    }
}

void PeerNetworkPrivate::handleSearchResults(const QString &username, const QByteArray &payload) const
{
    // Cheap check first: results for searches we stopped collecting are dropped uninflated.
    const std::optional<peer::FileSearchResponse::Header> header = peer::FileSearchResponse::peek(payload);
    if (!header || !openSearches.contains(header->token)) {
        qCDebug(lcSlskPeer).noquote() << "dropped search results from" << username << "(token not open)";
        return;
    }
    const std::optional<peer::FileSearchResponse> response = peer::FileSearchResponse::decode(payload);
    if (!response) {
        qCDebug(lcSlskPeer).noquote() << "dropped malformed search results from" << username;
        return;
    }
    Q_EMIT q->searchResponseReceived(username, *response);
}

void PeerNetworkPrivate::handleUserInfo(const QString &username, const QByteArray &payload)
{
    if (!pendingUserInfo.remove(username)) {
        return; // the frame cap already refuses unsolicited ones
    }
    const std::optional<peer::UserInfoResponse> info = peer::UserInfoResponse::decode(payload);
    if (info) {
        Q_EMIT q->userInfoReceived(username, *info);
    } else {
        Q_EMIT q->requestFailed(username, PeerRequest::UserInfo, 0, PeerError::ProtocolError);
    }
}

void PeerNetworkPrivate::handleFolderContents(const QString &username, const QByteArray &payload)
{
    const std::optional<peer::FolderContentsResponse> contents = peer::FolderContentsResponse::decode(payload);
    if (!contents) {
        qCDebug(lcSlskPeer).noquote() << "dropped malformed folder contents from" << username;
        return;
    }
    const auto pending = pendingFolders.constFind(contents->token);
    if (pending == pendingFolders.constEnd() || pending->username != username) {
        qCDebug(lcSlskPeer).noquote() << "dropped folder contents nobody asked for, from" << username;
        return;
    }
    pendingFolders.erase(pending);
    Q_EMIT q->folderContentsReceived(username, *contents);
}

void PeerNetworkPrivate::forwardTransferMessage(const QString &username, const Frame &f)
{
    const Decoded<PeerMessage> decoded = decodePeer(f.code, f.payload);
    if (decoded.status == DecodeStatus::Malformed) {
        qCWarning(lcSlskPeer).noquote() << "dropped malformed peer message from" << username << "code" << f.code;
        return;
    }
    if (!decoded.ok() || !decoded.message) {
        qCDebug(lcSlskPeer).noquote() << "skipped peer message" << f.code << "from" << username << "("
                                      << decodeStatusName(decoded.status) << ")";
        return;
    }
    std::visit(
        [this, &username](const auto &message) {
            using M = std::decay_t<decltype(message)>;
            if constexpr (std::is_same_v<M, peer::TransferRequest>) {
                Q_EMIT q->transferRequestReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::TransferResponse>) {
                Q_EMIT q->transferResponseReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::QueueUpload>) {
                Q_EMIT q->queueUploadReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::PlaceInQueueRequest>) {
                Q_EMIT q->placeInQueueRequestReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::PlaceInQueueResponse>) {
                Q_EMIT q->placeInQueueResponseReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::UploadFailed>) {
                Q_EMIT q->uploadFailedReceived(username, message);
            } else if constexpr (std::is_same_v<M, peer::UploadDenied>) {
                Q_EMIT q->uploadDeniedReceived(username, message);
            } else {
                qCDebug(lcSlskPeer) << "unexpected peer message type";
            }
        },
        *decoded.message);
}

void PeerNetworkPrivate::handleShareList(const QString &username, const QByteArray &payload)
{
    if (!pendingBrowse.remove(username)) {
        return; // the frame cap already refuses unsolicited ones; a late duplicate is ignored
    }
    if (payload.size() <= kWorkerDecodeThreshold) {
        const std::optional<peer::SharedFileListResponse> response = peer::SharedFileListResponse::decode(payload);
        if (response) {
            Q_EMIT q->sharedFileListReceived(username, response->list);
        } else {
            Q_EMIT q->requestFailed(username, PeerRequest::SharedFileList, 0, PeerError::ProtocolError);
        }
        return;
    }
    // A big list: inflate and parse it off the engine thread. The watcher is a child of the PeerNetwork, so a result
    // that arrives after it's gone goes nowhere.
    const auto promise = std::make_shared<QPromise<ShareListResult>>();
    auto *watcher = new QFutureWatcher<ShareListResult>(q);
    QObject::connect(watcher, &QFutureWatcherBase::finished, q, [this, watcher, username] {
        const ShareListResult result = watcher->future().resultCount() > 0 ? watcher->result() : std::nullopt;
        watcher->deleteLater();
        if (result) {
            Q_EMIT q->sharedFileListReceived(username, *result);
        } else {
            Q_EMIT q->requestFailed(username, PeerRequest::SharedFileList, 0, PeerError::ProtocolError);
        }
    });
    watcher->setFuture(promise->future());
    promise->start();
    QThreadPool::globalInstance()->start([promise, payload] {
        std::optional<peer::SharedFileListResponse> response = peer::SharedFileListResponse::decode(payload);
        promise->addResult(response ? ShareListResult(std::move(response->list)) : ShareListResult());
        promise->finish();
    });
}

void PeerNetworkPrivate::onBytesWritten(quint64 id)
{
    Connection *c = find(id);
    if (c == nullptr) {
        return;
    }
    touch(*c);
    if (c->closeWhenFlushed && c->socket->bytesToWrite() == 0) {
        close(id, PeerError::None, true);
    }
}

// --- sending ---------------------------------------------------------------------------------------------------------

void PeerNetworkPrivate::write(Connection &c, const QByteArray &bytes) const
{
    c.socket->write(bytes);
    touch(c);
}

void PeerNetworkPrivate::answer(Connection &c, const QByteArray &bytes) const
{
    if (c.socket->bytesToWrite() > kMaxUnsentAnswers) {
        qCWarning(lcSlskPeer).noquote() << "not answering" << c.username << ": too much unsent data already";
        return;
    }
    write(c, bytes);
}

void PeerNetworkPrivate::send(const QString &username, ConnectionType type, Pending pending, ConnectPurpose purpose)
{
    const PeerKey key{username, type};
    if (Connection *c = readyPrimary(key)) {
        if (!pending.closeAfter) {
            c->closeWhenFlushed = false; // the connection is in use for more than a search answer now
        }
        write(*c, pending.frame);
        if (pending.done) {
            pending.done(PeerError::None);
        }
        return;
    }
    if (const auto it = attemptByKey.constFind(key); it != attemptByKey.constEnd()) {
        attempts.at(it.value()).queue.append(std::move(pending));
        return;
    }
    Attempt attempt;
    attempt.username = username;
    attempt.type = type;
    attempt.purpose = purpose;
    attempt.queue.append(std::move(pending));
    startAttempt(std::move(attempt));
}

} // namespace internal

// --- PeerNetwork -----------------------------------------------------------------------------------------------------

using internal::Pending;

PeerNetwork::PeerNetwork(ServerSession &server, PermitSource &permits, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<internal::PeerNetworkPrivate>(this, server, permits))
{
    registerMetaTypes();
    connect(d->listener, &QTcpServer::newConnection, this, [this] { d->onNewConnection(); });
    connect(d->sweep, &QTimer::timeout, this, [this] { d->onSweep(); });
    connect(&server, &ServerSession::peerAddressReceived, this,
            [this](const fromserver::PeerAddress &address) { d->onPeerAddress(address); });
    connect(&server, &ServerSession::cantConnectToPeerReceived, this,
            [this](quint32 token) { d->onCantConnect(token); });
    connect(&server, &ServerSession::connectToPeerRequested, this,
            [this](const fromserver::ConnectToPeerRequest &request) { d->onConnectToPeerRequested(request); });
    // Without a server nobody can broker a connection: attempts waiting on it fail. Open connections stay.
    connect(&server, &ServerSession::ended, this, [this] { d->failAllAttempts(PeerError::NotLoggedIn); });
    d->restartSweep();
}

PeerNetwork::~PeerNetwork()
{
    // Pending callbacks are dropped, not called: their owners may be gone already.
    for (auto &[id, connection] : d->connections) {
        connection.socket->disconnect(this);
        connection.socket->abort();
    }
}

void PeerNetwork::setTimeouts(const Timeouts &timeouts)
{
    d->timeouts = timeouts;
    d->restartSweep();
}

void PeerNetwork::setSocketLimit(int sockets)
{
    d->socketLimit = std::clamp(sockets, 1, protocol::kSocketsMax);
}

void PeerNetwork::setShareProvider(ShareProvider *provider)
{
    d->shares = provider;
}

void PeerNetwork::setUserInfoProvider(UserInfoProvider *provider)
{
    d->userInfo = provider;
}

bool PeerNetwork::listen(quint16 port, QString *error)
{
    return listen(port, QHostAddress(QHostAddress::AnyIPv4), error);
}

bool PeerNetwork::listen(quint16 port, const QHostAddress &address, QString *error)
{
    d->listener->close();
    if (!d->listener->listen(address, port)) {
        if (error != nullptr) {
            *error = d->listener->errorString();
        }
        return false;
    }
    return true;
}

void PeerNetwork::stopListening()
{
    d->listener->close();
}

bool PeerNetwork::isListening() const
{
    return d->listener->isListening();
}

quint16 PeerNetwork::listenPort() const
{
    return d->listener->isListening() ? d->listener->serverPort() : 0;
}

bool PeerNetwork::requestSharedFileList(const QString &username, governor::Permit browse)
{
    if (username.isEmpty() || !browse.isValid() || browse.operation() != governor::Operation::Browse ||
        !browse.consume(governor::Operation::Browse)) {
        return false;
    }
    if (d->pendingBrowse.contains(username)) {
        return true; // the answer we're already waiting for serves this request too
    }
    d->pendingBrowse.insert(username, {});
    const QPointer<PeerNetwork> guard(this);
    d->send(username, ConnectionType::Peer,
            Pending{frame(peer::GetShareFileList{}),
                    [guard, username](PeerError error) {
                        if (!guard) {
                            return;
                        }
                        auto &pending = guard->d->pendingBrowse;
                        if (error == PeerError::None) {
                            if (const auto it = pending.find(username); it != pending.end()) {
                                it->deadline.setRemainingTime(guard->d->timeouts.response);
                            }
                        } else if (pending.remove(username) > 0) {
                            Q_EMIT guard->requestFailed(username, PeerRequest::SharedFileList, 0, error);
                        }
                    },
                    false},
            ConnectPurpose::Request);
    return true;
}

bool PeerNetwork::requestUserInfo(const QString &username, governor::Permit userInfo)
{
    if (username.isEmpty() || !userInfo.isValid() || userInfo.operation() != governor::Operation::UserInfo ||
        !userInfo.consume(governor::Operation::UserInfo)) {
        return false;
    }
    if (d->pendingUserInfo.contains(username)) {
        return true;
    }
    d->pendingUserInfo.insert(username, {});
    const QPointer<PeerNetwork> guard(this);
    d->send(username, ConnectionType::Peer,
            Pending{frame(peer::UserInfoRequest{}),
                    [guard, username](PeerError error) {
                        if (!guard) {
                            return;
                        }
                        auto &pending = guard->d->pendingUserInfo;
                        if (error == PeerError::None) {
                            if (const auto it = pending.find(username); it != pending.end()) {
                                it->deadline.setRemainingTime(guard->d->timeouts.response);
                            }
                        } else if (pending.remove(username) > 0) {
                            Q_EMIT guard->requestFailed(username, PeerRequest::UserInfo, 0, error);
                        }
                    },
                    false},
            ConnectPurpose::Request);
    return true;
}

quint32 PeerNetwork::requestFolderContents(const QString &username, const RemotePath &folder,
                                           governor::Permit folderContents)
{
    if (username.isEmpty() || !folderContents.isValid() ||
        folderContents.operation() != governor::Operation::FolderContents ||
        !folderContents.consume(governor::Operation::FolderContents)) {
        return 0;
    }
    const quint32 token = d->folderTokens.next([this](quint32 t) { return d->pendingFolders.contains(t); });
    d->pendingFolders.insert(token, internal::PendingFolder{username, folder, QDeadlineTimer::Forever});
    const QPointer<PeerNetwork> guard(this);
    d->send(username, ConnectionType::Peer,
            Pending{frame(peer::FolderContentsRequest{token, folder}),
                    [guard, username, token](PeerError error) {
                        if (!guard) {
                            return;
                        }
                        auto &pending = guard->d->pendingFolders;
                        if (error == PeerError::None) {
                            if (const auto it = pending.find(token); it != pending.end()) {
                                it->deadline.setRemainingTime(guard->d->timeouts.response);
                            }
                        } else if (pending.remove(token) > 0) {
                            Q_EMIT guard->requestFailed(username, PeerRequest::FolderContents, token, error);
                        }
                    },
                    false},
            ConnectPurpose::Request);
    return token;
}

void PeerNetwork::openSearch(quint32 token)
{
    d->openSearches.insert(token);
}

void PeerNetwork::closeSearch(quint32 token)
{
    d->openSearches.remove(token);
}

bool PeerNetwork::isSearchOpen(quint32 token) const
{
    return d->openSearches.contains(token);
}

void PeerNetwork::sendSearchResponse(const QString &username, const peer::FileSearchResponse &response,
                                     SendCallback done)
{
    if (username.isEmpty()) {
        if (done) {
            done(PeerError::ProtocolError);
        }
        return;
    }
    d->send(username, ConnectionType::Peer, Pending{frame(response), std::move(done), true},
            ConnectPurpose::SearchResponse);
}

void PeerNetwork::sendPeerMessage(const QString &username, const QByteArray &bytes, ConnectPurpose purpose,
                                  SendCallback done)
{
    if (username.isEmpty()) {
        if (done) {
            done(PeerError::ProtocolError);
        }
        return;
    }
    d->send(username, ConnectionType::Peer, Pending{bytes, std::move(done), false}, purpose);
}

bool PeerNetwork::hasPeerConnection(const QString &username) const
{
    const internal::PeerKey key{username, ConnectionType::Peer};
    return d->readyPrimary(key) != nullptr || d->attemptByKey.contains(key);
}

quint64 PeerNetwork::openFileConnection(const QString &username)
{
    internal::Attempt attempt;
    attempt.username = username;
    attempt.type = ConnectionType::File;
    attempt.purpose = ConnectPurpose::Transfer;
    const quint64 request = d->nextId++;
    attempt.fileRequest = request;
    d->startAttempt(std::move(attempt));
    return request;
}

void PeerNetwork::cancelFileConnection(quint64 id)
{
    for (const auto &[attemptId, attempt] : d->attempts) {
        if (attempt.type == ConnectionType::File && attempt.fileRequest == id) {
            qCDebug(lcSlskPeer).noquote() << "file connection to" << attempt.username << "cancelled";
            d->takeAttempt(attemptId); // invalidates `attempt`
            return;
        }
    }
}

void PeerNetwork::setFileConnectionFilter(std::function<bool(const QString &)> filter)
{
    d->fileFilter = std::move(filter);
}

void PeerNetwork::openDistributedConnection(const QString &username, const QHostAddress &address, quint16 port)
{
    const internal::PeerKey key{username, ConnectionType::Distributed};
    if (d->readyPrimary(key) != nullptr || d->attemptByKey.contains(key)) {
        return;
    }
    internal::Attempt attempt;
    attempt.username = username;
    attempt.type = ConnectionType::Distributed;
    attempt.purpose = ConnectPurpose::Distributed;
    attempt.knownAddress = std::make_pair(address, port);
    d->startAttempt(std::move(attempt));
}

void PeerNetwork::sendDistributedMessage(const QString &username, const QByteArray &bytes)
{
    if (internal::Connection *c = d->readyPrimary(internal::PeerKey{username, ConnectionType::Distributed})) {
        d->write(*c, bytes);
    } else {
        qCDebug(lcSlskDistrib).noquote() << "no distributed connection to" << username << "; message dropped";
    }
}

void PeerNetwork::closeDistributedConnection(const QString &username)
{
    const internal::PeerKey key{username, ConnectionType::Distributed};
    if (const auto attempt = d->attemptByKey.constFind(key); attempt != d->attemptByKey.constEnd()) {
        d->failAttempt(attempt.value(), PeerError::Cancelled);
    }
    QList<quint64> ids;
    for (const auto &[id, connection] : d->connections) {
        if (connection.type == ConnectionType::Distributed && connection.username == username) {
            ids.append(id);
        }
    }
    for (const quint64 id : std::as_const(ids)) {
        d->close(id, PeerError::Cancelled);
    }
}

void PeerNetwork::setDistributedConnectionFilter(std::function<bool(const QString &)> filter)
{
    d->distributedFilter = std::move(filter);
}

void PeerNetwork::closeAll()
{
    d->failAllAttempts(PeerError::Cancelled);
    QList<quint64> ids;
    for (const auto &[id, connection] : d->connections) {
        ids.append(id);
    }
    for (const quint64 id : std::as_const(ids)) {
        d->onClosed(id);
    }
    const QStringList browses = d->pendingBrowse.keys();
    d->pendingBrowse.clear();
    for (const QString &user : browses) {
        Q_EMIT requestFailed(user, PeerRequest::SharedFileList, 0, PeerError::Cancelled);
    }
    const QStringList infos = d->pendingUserInfo.keys();
    d->pendingUserInfo.clear();
    for (const QString &user : infos) {
        Q_EMIT requestFailed(user, PeerRequest::UserInfo, 0, PeerError::Cancelled);
    }
    const auto folders = std::exchange(d->pendingFolders, {});
    for (auto it = folders.constBegin(); it != folders.constEnd(); ++it) {
        Q_EMIT requestFailed(it->username, PeerRequest::FolderContents, it.key(), PeerError::Cancelled);
    }
    d->completedTokens.clear();
}

int PeerNetwork::socketCount() const
{
    return d->socketCount();
}

int PeerNetwork::connectionCount(ConnectionType type) const
{
    int count = 0;
    for (const auto &[id, connection] : d->connections) {
        if (connection.ready && connection.type == type) {
            ++count;
        }
    }
    return count;
}

} // namespace selecta::slsk
