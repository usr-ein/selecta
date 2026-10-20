// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/DistributedMessages.h"
#include "slsk/Protocol.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace selecta::slsk {

class PeerNetwork;
class ServerSession;

namespace internal {
class DistributedNodePrivate;
} // namespace internal

// Our place in the distributed search network (references/soulseek-protocol.md §8). Without a parent, nobody's global
// searches reach us, so membership is part of being a good citizen, not an extra.
//
// - After login: HaveNoParent(true), BranchRoot(self), BranchLevel(0), AcceptChildren(false).
// - The server's PossibleParents: connect `D` to every candidate (up to 10; each connection asks the PermitSource,
//   purpose Distributed). Parent selection follows Nicotine+ and SoulseekQt: the first candidate that sends a
//   DistribSearch after its branch level and root are known becomes the parent; the other candidates are closed.
// - With a parent: report HaveNoParent(false), BranchLevel(parent + 1) and BranchRoot to the server and to children,
//   and pass on later level and root changes. Only the parent may send distributed messages; anyone else is closed,
//   as is a parent that sends a negative level.
// - EmbeddedMessage (code 93) from the server makes us a branch root (level 0, root ourselves); code 93 from a parent
//   is unpacked the same way. Only an embedded DistribSearch counts.
// - Every search is forwarded to the children as the raw bytes received (never re-encoded), and reaches core/ as
//   searchRequestReceived(). A search identical to the previous one is dropped (a cheap loop guard).
// - Children are accepted while we have a parent or are a root, up to min(avgspeed / ParentSpeedRatio / 100, 10) when
//   our server-side average speed reaches ParentMinSpeed (the Nicotine+ rule), and only if setChildrenAllowed().
//   They get our level and root on connect. AcceptChildren follows whether there is room.
// - Losing the parent: back to the first step; children are kept. ResetDistributed (code 130): parent, candidates and
//   children are all dropped, then the first step.
//
// Threading: lives on the engine thread and is used only from it. Ownership: a QObject. Not owned: the ServerSession
// and PeerNetwork (they must outlive it). Malformed distributed messages are dropped and logged
// (selecta.slsk.distrib); protocol violations close the connection.
class DistributedNode : public QObject
{
    Q_OBJECT

public:
    DistributedNode(ServerSession &server, PeerNetwork &peers, QObject *parent = nullptr);
    ~DistributedNode() override;
    DistributedNode(const DistributedNode &) = delete;
    DistributedNode &operator=(const DistributedNode &) = delete;

    // Whether we take children at all (a setting in core/). Default true; the server's rules still apply.
    void setChildrenAllowed(bool allowed);

    bool hasParent() const;
    QString parentUsername() const; // empty without a parent
    bool isBranchRoot() const;      // the server feeds us searches directly
    int branchLevel() const;
    QString branchRoot() const;
    QStringList childUsernames() const; // QObject::children() is something else
    int maxChildren() const;

Q_SIGNALS:
    // A search from the distributed network, already forwarded to our children.
    void searchRequestReceived(const selecta::slsk::distrib::Search &search);
    // Our parent changed (empty: none) or our branch moved.
    void parentChanged(const QString &parentUsername);
    void branchChanged(int level, const QString &root);
    void childrenChanged(int count);

private:
    friend class internal::DistributedNodePrivate;
    std::unique_ptr<internal::DistributedNodePrivate> d;
};

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::distrib::Search)
