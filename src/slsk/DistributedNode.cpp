// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/DistributedNode.h"

#include "slsk/Frame.h"
#include "slsk/Logging.h"
#include "slsk/MetaTypes.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"

#include <QHash>
#include <QPointer>

#include <algorithm>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::slsk {
namespace internal {
namespace {

// A parent candidate we connected to (or are connecting to), and what it told us so far.
struct Candidate
{
    std::optional<qint32> level;
    std::optional<QString> root;
};

QByteArray distribFrame(DistribCode code, QByteArrayView payload)
{
    return encodeFrame(CodeWidth::Byte, static_cast<quint32>(code), payload);
}

// The search inside a message, if it carries one: a Search, or an EmbeddedMessage wrapping one.
std::optional<QByteArray> searchPayload(const DistribMessage &message, const QByteArray &payload)
{
    if (std::holds_alternative<distrib::Search>(message)) {
        return payload;
    }
    if (const auto *embedded = std::get_if<distrib::EmbeddedMessage>(&message);
        embedded != nullptr && embedded->distribCode == static_cast<quint8>(DistribCode::Search)) {
        return embedded->payload;
    }
    return std::nullopt;
}

} // namespace

class DistributedNodePrivate
{
public:
    DistributedNodePrivate(DistributedNode *owner, ServerSession &session, PeerNetwork &network)
        : q(owner)
        , server(session)
        , peers(network)
    {}

    // --- server ------------------------------------------------------------------------------------------------------
    void onLoggedIn();
    void onEnded();
    void onPossibleParents(const QList<ParentCandidate> &list);
    void onServerEmbedded(quint8 code, const QByteArray &payload);
    void onReset();
    void onOwnStats(const QString &username, const UserStats &stats);

    // --- D connections -----------------------------------------------------------------------------------------------
    void onConnected(const QString &username, bool outbound);
    void onConnectFailed(const QString &username);
    void onMessage(const QString &username, quint32 code, const QByteArray &payload);
    void onDisconnected(const QString &username);

    // --- the tree ----------------------------------------------------------------------------------------------------
    void candidateMessage(const QString &username, const DistribMessage &message, const QByteArray &payload);
    void parentMessage(const DistribMessage &message, const QByteArray &payload);
    void adoptParent(const QString &username, const Candidate &candidate);
    void loseParent();
    void announceNoParent();
    void setBranch(qint32 newLevel, const QString &newRoot);
    void deliverSearch(const QByteArray &payload);
    void acceptChild(const QString &username);
    void dropChildren();
    void closeCandidates();
    void close(const QString &username, const char *why);
    int maxChildren() const;
    bool canTakeChild() const;
    void updateAcceptChildren();

    DistributedNode *q;
    ServerSession &server;
    PeerNetwork &peers;
    QString parent;
    QHash<QString, Candidate> candidates;
    QStringList children; // in the order they joined
    qint32 level = 0;
    QString root;
    bool branchRoot = false; // the server feeds us searches (EmbeddedMessage)
    bool childrenAllowed = true;
    std::optional<bool> acceptingSent; // the last AcceptChildren the server got
    quint32 parentMinSpeed = 1;        // the server's presumed defaults until it says (references §8)
    quint32 parentSpeedRatio = 50;
    quint32 averageSpeed = 0; // ours, as the server computes it
    QByteArray lastSearch;    // the duplicate guard
};

// --- server ----------------------------------------------------------------------------------------------------------

void DistributedNodePrivate::onLoggedIn()
{
    // A new session starts the tree over (protocol §8 step 1).
    closeCandidates();
    dropChildren();
    if (!parent.isEmpty()) {
        const QString old = std::exchange(parent, {});
        peers.closeDistributedConnection(old);
        Q_EMIT q->parentChanged({});
    }
    branchRoot = false;
    acceptingSent.reset();
    lastSearch.clear();
    level = -1; // forces both reports below
    root.clear();
    announceNoParent();
    updateAcceptChildren();
}

void DistributedNodePrivate::onEnded()
{
    // Without a server nobody can answer the searches: leave the tree.
    closeCandidates();
    dropChildren();
    if (!parent.isEmpty()) {
        const QString old = std::exchange(parent, {});
        peers.closeDistributedConnection(old);
        Q_EMIT q->parentChanged({});
    }
    branchRoot = false;
}

void DistributedNodePrivate::onPossibleParents(const QList<ParentCandidate> &list)
{
    if (!parent.isEmpty() || branchRoot || !server.isLoggedIn()) {
        return;
    }
    for (const ParentCandidate &candidate : list) {
        if (candidates.size() >= protocol::kParentCandidatesMax) {
            break;
        }
        const bool usable = !candidate.username.isEmpty() && candidate.username != server.username() &&
                            candidate.ip != 0 && candidate.port != 0 && candidate.port <= 65535;
        if (!usable || candidates.contains(candidate.username) || children.contains(candidate.username)) {
            continue;
        }
        candidates.insert(candidate.username, Candidate{});
        qCDebug(lcSlskDistrib).noquote() << "trying parent candidate" << candidate.username;
        peers.openDistributedConnection(candidate.username, ipv4Address(candidate.ip),
                                        static_cast<quint16>(candidate.port));
    }
}

void DistributedNodePrivate::onServerEmbedded(quint8 code, const QByteArray &payload)
{
    // The server feeds branch roots directly; with a peer parent, searches come from it instead (Nicotine+).
    if (code != static_cast<quint8>(DistribCode::Search) || !parent.isEmpty()) {
        return;
    }
    if (!branchRoot) {
        branchRoot = true;
        closeCandidates();
        qCInfo(lcSlskDistrib) << "we are a branch root";
        updateAcceptChildren();
    }
    deliverSearch(payload);
}

void DistributedNodePrivate::onReset()
{
    qCInfo(lcSlskDistrib) << "the server reset our place in the distributed network";
    closeCandidates();
    dropChildren();
    if (!parent.isEmpty()) {
        loseParent();
    } else {
        branchRoot = false;
        announceNoParent();
        updateAcceptChildren();
    }
}

void DistributedNodePrivate::onOwnStats(const QString &username, const UserStats &stats)
{
    if (username != server.username() || stats.averageSpeed == averageSpeed) {
        return;
    }
    averageSpeed = stats.averageSpeed;
    updateAcceptChildren();
}

// --- D connections ---------------------------------------------------------------------------------------------------

void DistributedNodePrivate::onConnected(const QString &username, bool outbound)
{
    if (outbound) {
        // One of our candidates (it sends its level and root next), or one we no longer want.
        if (!candidates.contains(username) && username != parent) {
            close(username, "no longer a candidate");
        }
        return;
    }
    // Someone connected to us: a would-be child.
    if (children.contains(username)) {
        return; // a second connection of the same child (both halves of a race)
    }
    if (username == parent || candidates.contains(username) || !canTakeChild()) {
        close(username, "not taking this child");
        return;
    }
    acceptChild(username);
}

void DistributedNodePrivate::onConnectFailed(const QString &username)
{
    candidates.remove(username);
}

void DistributedNodePrivate::onMessage(const QString &username, quint32 code, const QByteArray &payload)
{
    const Decoded<DistribMessage> decoded = decodeDistrib(code, payload);
    if (decoded.status == DecodeStatus::Ignored) {
        return; // DistribPing and ChildDepth: obsolete, harmless from anyone
    }
    if (decoded.status != DecodeStatus::Decoded || !decoded.message) {
        qCDebug(lcSlskDistrib).noquote() << "dropped distributed code" << code << "from" << username;
        return;
    }
    if (username == parent) {
        parentMessage(*decoded.message, payload);
    } else if (candidates.contains(username)) {
        candidateMessage(username, *decoded.message, payload);
    } else {
        // Only the parent (and the candidates) may send distributed messages (Nicotine+).
        const bool wasChild = children.removeAll(username) > 0;
        close(username, "distributed message from a peer that isn't our parent");
        if (wasChild) {
            updateAcceptChildren();
            Q_EMIT q->childrenChanged(static_cast<int>(children.size()));
        }
    }
}

void DistributedNodePrivate::onDisconnected(const QString &username)
{
    if (username == parent) {
        qCInfo(lcSlskDistrib).noquote() << "lost our parent" << username;
        loseParent();
        return;
    }
    if (candidates.remove(username) > 0) {
        return;
    }
    if (children.removeAll(username) > 0) {
        updateAcceptChildren();
        Q_EMIT q->childrenChanged(static_cast<int>(children.size()));
    }
}

// --- the tree --------------------------------------------------------------------------------------------------------

void DistributedNodePrivate::candidateMessage(const QString &username, const DistribMessage &message,
                                              const QByteArray &payload)
{
    Candidate &candidate = candidates[username];
    if (const auto *levelMessage = std::get_if<distrib::BranchLevel>(&message)) {
        if (levelMessage->level < 0) {
            candidates.remove(username);
            close(username, "negative branch level");
            return;
        }
        candidate.level = levelMessage->level;
        return;
    }
    if (const auto *rootMessage = std::get_if<distrib::BranchRoot>(&message)) {
        candidate.root = rootMessage->root;
        return;
    }
    // The first candidate to send a search once its level and root are known becomes the parent (Nicotine+,
    // SoulseekQt). A level-0 candidate is itself the root and may not say so.
    const std::optional<QByteArray> search = searchPayload(message, payload);
    const bool rootKnown = candidate.root.has_value() || candidate.level == 0;
    if (search && candidate.level && rootKnown) {
        const Candidate chosen = candidates.take(username);
        adoptParent(username, chosen);
        deliverSearch(*search);
    }
}

void DistributedNodePrivate::parentMessage(const DistribMessage &message, const QByteArray &payload)
{
    if (const auto *levelMessage = std::get_if<distrib::BranchLevel>(&message)) {
        if (levelMessage->level < 0) {
            qCInfo(lcSlskDistrib).noquote() << "parent" << parent << "sent a negative branch level";
            loseParent();
            return;
        }
        setBranch(levelMessage->level + 1, root);
        return;
    }
    if (const auto *rootMessage = std::get_if<distrib::BranchRoot>(&message)) {
        setBranch(level, rootMessage->root);
        return;
    }
    if (const std::optional<QByteArray> search = searchPayload(message, payload)) {
        deliverSearch(*search);
    }
}

void DistributedNodePrivate::adoptParent(const QString &username, const Candidate &candidate)
{
    parent = username;
    branchRoot = false;
    closeCandidates();
    qCInfo(lcSlskDistrib).noquote() << "parent:" << username << "at level" << candidate.level.value_or(0);
    server.sendHaveNoParent(false);
    setBranch(candidate.level.value_or(0) + 1, candidate.root.value_or(username));
    updateAcceptChildren();
    Q_EMIT q->parentChanged(parent);
}

void DistributedNodePrivate::loseParent()
{
    // Back to step 1. Children stay; they hear our new level and root.
    const QString old = std::exchange(parent, {});
    branchRoot = false;
    if (!old.isEmpty()) {
        peers.closeDistributedConnection(old);
    }
    announceNoParent();
    updateAcceptChildren();
    Q_EMIT q->parentChanged({});
}

void DistributedNodePrivate::announceNoParent()
{
    server.sendHaveNoParent(true);
    setBranch(0, server.username());
}

void DistributedNodePrivate::setBranch(qint32 newLevel, const QString &newRoot)
{
    const bool levelChanged = newLevel != level;
    const bool rootChanged = newRoot != root;
    if (!levelChanged && !rootChanged) {
        return;
    }
    level = newLevel;
    root = newRoot;
    if (rootChanged) {
        server.sendBranchRoot(root);
    }
    if (levelChanged) {
        server.sendBranchLevel(static_cast<quint32>(level));
    }
    const QByteArray levelFrame = frame(distrib::BranchLevel{level});
    const QByteArray rootFrame = frame(distrib::BranchRoot{root});
    for (const QString &child : std::as_const(children)) {
        if (levelChanged) {
            peers.sendDistributedMessage(child, levelFrame);
        }
        if (rootChanged) {
            peers.sendDistributedMessage(child, rootFrame);
        }
    }
    Q_EMIT q->branchChanged(level, root);
}

void DistributedNodePrivate::deliverSearch(const QByteArray &payload)
{
    if (payload == lastSearch) {
        return; // the same search twice in a row: a loop or a duplicate
    }
    std::optional<distrib::Search> search = distrib::Search::decode(payload);
    if (!search) {
        qCDebug(lcSlskDistrib) << "dropped a malformed search";
        return;
    }
    lastSearch = payload;
    // Forwarded as the bytes received, never re-encoded (protocol §8): unknown trailing fields travel on.
    const QByteArray forwarded = distribFrame(DistribCode::Search, payload);
    for (const QString &child : std::as_const(children)) {
        peers.sendDistributedMessage(child, forwarded);
    }
    Q_EMIT q->searchRequestReceived(*search);
}

void DistributedNodePrivate::acceptChild(const QString &username)
{
    children.append(username);
    qCInfo(lcSlskDistrib).noquote() << "child:" << username;
    peers.sendDistributedMessage(username, frame(distrib::BranchLevel{level}));
    peers.sendDistributedMessage(username, frame(distrib::BranchRoot{root}));
    updateAcceptChildren();
    Q_EMIT q->childrenChanged(static_cast<int>(children.size()));
}

void DistributedNodePrivate::dropChildren()
{
    const QStringList gone = std::exchange(children, {});
    for (const QString &child : gone) {
        peers.closeDistributedConnection(child);
    }
    if (!gone.isEmpty()) {
        Q_EMIT q->childrenChanged(0);
    }
}

void DistributedNodePrivate::closeCandidates()
{
    // Taken first: closing reports back synchronously.
    const QStringList names = std::exchange(candidates, {}).keys();
    for (const QString &name : names) {
        peers.closeDistributedConnection(name);
    }
}

void DistributedNodePrivate::close(const QString &username, const char *why)
{
    qCDebug(lcSlskDistrib).noquote() << "closing distributed connection to" << username << ":" << why;
    peers.closeDistributedConnection(username);
}

int DistributedNodePrivate::maxChildren() const
{
    // Nicotine+'s rule: min(avgspeed / ratio / 100, 10) once our average speed reaches ParentMinSpeed.
    if (!childrenAllowed || parentSpeedRatio == 0 || averageSpeed < parentMinSpeed) {
        return 0;
    }
    return static_cast<int>(std::min<quint32>(averageSpeed / parentSpeedRatio / 100, protocol::kDistribChildrenMax));
}

bool DistributedNodePrivate::canTakeChild() const
{
    return (!parent.isEmpty() || branchRoot) && children.size() < maxChildren();
}

void DistributedNodePrivate::updateAcceptChildren()
{
    const bool accepting = canTakeChild();
    if (acceptingSent != accepting && server.sendAcceptChildren(accepting)) {
        acceptingSent = accepting;
    }
}

} // namespace internal

// --- DistributedNode -------------------------------------------------------------------------------------------------

DistributedNode::DistributedNode(ServerSession &server, PeerNetwork &peers, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<internal::DistributedNodePrivate>(this, server, peers))
{
    registerMetaTypes();
    connect(&server, &ServerSession::loggedIn, this, [this] { d->onLoggedIn(); });
    connect(&server, &ServerSession::ended, this, [this] { d->onEnded(); });
    connect(&server, &ServerSession::possibleParentsReceived, this,
            [this](const QList<ParentCandidate> &candidates) { d->onPossibleParents(candidates); });
    connect(&server, &ServerSession::embeddedMessageReceived, this,
            [this](quint8 code, const QByteArray &payload) { d->onServerEmbedded(code, payload); });
    connect(&server, &ServerSession::resetDistributedReceived, this, [this] { d->onReset(); });
    connect(&server, &ServerSession::parentMinSpeedReceived, this, [this](quint32 speed) {
        d->parentMinSpeed = speed;
        d->updateAcceptChildren();
    });
    connect(&server, &ServerSession::parentSpeedRatioReceived, this, [this](quint32 ratio) {
        d->parentSpeedRatio = ratio;
        d->updateAcceptChildren();
    });
    connect(&server, &ServerSession::watchUserReplyReceived, this,
            [this](const fromserver::WatchUserReply &reply) { d->onOwnStats(reply.username, reply.stats); });
    connect(&server, &ServerSession::userStatsReceived, this,
            [this](const fromserver::UserStatsUpdate &update) { d->onOwnStats(update.username, update.stats); });
    connect(&peers, &PeerNetwork::distributedConnected, this,
            [this](const QString &username, bool outbound) { d->onConnected(username, outbound); });
    connect(&peers, &PeerNetwork::distributedConnectFailed, this,
            [this](const QString &username) { d->onConnectFailed(username); });
    connect(&peers, &PeerNetwork::distributedMessageReceived, this,
            [this](const QString &username, quint32 code, const QByteArray &payload) {
                d->onMessage(username, code, payload);
            });
    connect(&peers, &PeerNetwork::distributedDisconnected, this,
            [this](const QString &username) { d->onDisconnected(username); });
    // A peer that picked us as its parent asks through the server when it can't reach us directly.
    peers.setDistributedConnectionFilter([guard = QPointer<DistributedNode>(this)](const QString &username) {
        return guard && guard->d->canTakeChild() && !guard->d->children.contains(username);
    });
}

DistributedNode::~DistributedNode()
{
    d->peers.setDistributedConnectionFilter({});
}

void DistributedNode::setChildrenAllowed(bool allowed)
{
    if (d->childrenAllowed == allowed) {
        return;
    }
    d->childrenAllowed = allowed;
    if (!allowed) {
        d->dropChildren();
    }
    d->updateAcceptChildren();
}

bool DistributedNode::hasParent() const
{
    return !d->parent.isEmpty();
}

QString DistributedNode::parentUsername() const
{
    return d->parent;
}

bool DistributedNode::isBranchRoot() const
{
    return d->branchRoot;
}

int DistributedNode::branchLevel() const
{
    return d->level;
}

QString DistributedNode::branchRoot() const
{
    return d->root;
}

QStringList DistributedNode::childUsernames() const
{
    return d->children;
}

int DistributedNode::maxChildren() const
{
    return d->maxChildren();
}

} // namespace selecta::slsk
