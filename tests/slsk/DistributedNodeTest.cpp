// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/DistributedNode.h"

#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"

#include <QSignalSpy>

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using namespace std::chrono_literals;
using selecta::governor::Operation;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::permit;
using selecta::slsk::testing::settle;
using selecta::slsk::testing::TestPermits;
using selecta::slsk::testing::waitFor;

namespace {

constexpr quint32 kLoopback = 0x7F00'0001;

QByteArray searchPayload(const QString &user, quint32 token, const QString &query)
{
    return distrib::Search{protocol::kDistribSearchIdentifier, user, token, query}.encode();
}

QByteArray searchFrame(const QByteArray &payload)
{
    return encodeFrame(CodeWidth::Byte, static_cast<quint32>(DistribCode::Search), payload);
}

class DistributedNodeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        // Fast enough for ten children: 10 MB/s, ratio 50 (protocol §8).
        server.setUserStats(u"selecta"_s, UserStats{10'000'000, 0, 0, 0, 0});
        session.setTimeouts({2s, 2s, 5min});
        peers.setTimeouts({1s, 1s, 5s, 1s, 3s});
        ASSERT_TRUE(peers.listen(0, QHostAddress(QHostAddress::LocalHost)));
        session.setListenPort(peers.listenPort());
        ASSERT_TRUE(session.connectToServer(server.address(), server.port(), permit(Operation::Login)));
        ASSERT_TRUE(session.login(u"selecta"_s, u"pw"_s));
        ASSERT_TRUE(waitFor([&] { return session.isLoggedIn(); }));
        ASSERT_TRUE(waitFor([&] { return !sent<toserver::AcceptChildren>().isEmpty(); }));
        ASSERT_TRUE(waitFor([&] { return node.maxChildren() == 10; }));
    }

    template <typename M>
    QList<M> sent() const
    {
        return server.receivedOf<M>(u"selecta"_s);
    }

    // A peer reached directly only: one D connection each way, so closing it ends the link.
    std::unique_ptr<FakePeer> makePeer(const QString &name)
    {
        auto peer = std::make_unique<FakePeer>(name, &server);
        peer->answerIndirect = false;
        EXPECT_TRUE(peer->start());
        return peer;
    }

    // Offers `candidates` as parents and returns, for each, the D connection we opened to it.
    QList<quint64> offerParents(const QList<FakePeer *> &candidates)
    {
        std::vector<std::unique_ptr<QSignalSpy>> spies;
        fromserver::PossibleParents offer;
        for (FakePeer *candidate : candidates) {
            spies.push_back(std::make_unique<QSignalSpy>(candidate, &FakePeer::identified));
            offer.candidates.append(ParentCandidate{candidate->username(), kLoopback, candidate->port()});
        }
        EXPECT_TRUE(server.sendTo(u"selecta"_s, offer));
        QList<quint64> connections;
        for (const auto &spy : spies) {
            EXPECT_TRUE(waitFor([&] { return !spy->isEmpty(); }));
            connections.append(spy->isEmpty() ? 0 : spy->at(0).at(0).toULongLong());
        }
        return connections;
    }

    // `peer` becomes our parent at `level` under `root`.
    quint64 adopt(FakePeer &peer, qint32 level, const QString &root)
    {
        const quint64 connection = offerParents({&peer}).value(0);
        peer.sendDistrib(connection, frame(distrib::BranchLevel{level}));
        peer.sendDistrib(connection, frame(distrib::BranchRoot{root}));
        peer.sendDistrib(connection, searchFrame(searchPayload(u"warmup"_s, 1, u"first search"_s)));
        EXPECT_TRUE(waitFor([&] { return node.parentUsername() == peer.username(); }));
        return connection;
    }

    // `peer` connects to us as a child; returns the connection.
    quint64 join(FakePeer &peer)
    {
        const quint64 connection =
            peer.connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Distributed);
        EXPECT_TRUE(waitFor([&] { return node.childUsernames().contains(peer.username()); }));
        return connection;
    }

    static QList<QByteArray> payloadsOf(const FakePeer &peer, quint64 connection, DistribCode code)
    {
        QList<QByteArray> payloads;
        for (const auto &[id, message] : peer.distribReceived) {
            if (id == connection && message.first == static_cast<quint32>(code)) {
                payloads.append(message.second);
            }
        }
        return payloads;
    }

    FakeServer server;
    ServerSession session;
    TestPermits permits;
    PeerNetwork peers{session, permits};
    DistributedNode node{session, peers};
};

} // namespace

TEST_F(DistributedNodeTest, AnnouncesNoParentAfterLogin)
{
    // HaveNoParent(true), BranchRoot(self), BranchLevel(0), AcceptChildren(false), in that order (protocol §8).
    QList<quint32> codes;
    const QList<ToServerMessage> received = server.received(u"selecta"_s);
    for (const ToServerMessage &message : received) {
        const quint32 code = std::visit([](const auto &m) { return static_cast<quint32>(m.kCode); }, message);
        if (code == static_cast<quint32>(ServerCode::HaveNoParent) ||
            code == static_cast<quint32>(ServerCode::BranchRoot) ||
            code == static_cast<quint32>(ServerCode::BranchLevel) ||
            code == static_cast<quint32>(ServerCode::AcceptChildren)) {
            codes.append(code);
        }
    }
    ASSERT_GE(codes.size(), 4);
    EXPECT_EQ(codes.mid(0, 4), (QList<quint32>{static_cast<quint32>(ServerCode::HaveNoParent),
                                               static_cast<quint32>(ServerCode::BranchRoot),
                                               static_cast<quint32>(ServerCode::BranchLevel),
                                               static_cast<quint32>(ServerCode::AcceptChildren)}));
    EXPECT_TRUE(sent<toserver::HaveNoParent>().at(0).noParent);
    EXPECT_EQ(sent<toserver::BranchRoot>().at(0).root, u"selecta"_s);
    EXPECT_EQ(sent<toserver::BranchLevel>().at(0).level, 0U);
    EXPECT_FALSE(sent<toserver::AcceptChildren>().at(0).accept); // no parent yet, however fast we are
    EXPECT_FALSE(node.hasParent());
    EXPECT_EQ(node.branchLevel(), 0);
    EXPECT_EQ(node.branchRoot(), u"selecta"_s);
}

TEST_F(DistributedNodeTest, FirstCandidateToSendASearchBecomesTheParent)
{
    const auto quiet = makePeer(u"quiet"_s);
    const auto chatty = makePeer(u"chatty"_s);
    const QSignalSpy parents(&node, &DistributedNode::parentChanged);
    const QSignalSpy searches(&node, &DistributedNode::searchRequestReceived);
    const QList<quint64> connections = offerParents({quiet.get(), chatty.get()});
    ASSERT_EQ(connections.size(), 2);
    EXPECT_EQ(permits.requestsFor(u"quiet"_s), 1); // each candidate costs a lookup, purpose Distributed
    EXPECT_EQ(permits.requests.constLast().second, ConnectPurpose::Distributed);
    // The quiet one tells its place but sends no search; the chatty one sends a search once its place is known.
    quiet->sendDistrib(connections.at(0), frame(distrib::BranchLevel{1}));
    quiet->sendDistrib(connections.at(0), frame(distrib::BranchRoot{u"top"_s}));
    chatty->sendDistrib(connections.at(1), searchFrame(searchPayload(u"early"_s, 9, u"too early"_s))); // ignored
    chatty->sendDistrib(connections.at(1), frame(distrib::BranchLevel{2}));
    chatty->sendDistrib(connections.at(1), frame(distrib::BranchRoot{u"summit"_s}));
    chatty->sendDistrib(connections.at(1), searchFrame(searchPayload(u"alice"_s, 7, u"kel varo"_s)));
    ASSERT_TRUE(waitFor([&] { return node.hasParent(); }));
    EXPECT_EQ(node.parentUsername(), u"chatty"_s);
    EXPECT_EQ(node.branchLevel(), 3);
    EXPECT_EQ(node.branchRoot(), u"summit"_s);
    ASSERT_EQ(parents.size(), 1);
    EXPECT_EQ(parents.at(0).at(0).toString(), u"chatty"_s);
    // The server hears where we are now.
    ASSERT_TRUE(waitFor([&] { return sent<toserver::BranchLevel>().constLast().level == 3; }));
    EXPECT_FALSE(sent<toserver::HaveNoParent>().constLast().noParent);
    EXPECT_EQ(sent<toserver::BranchRoot>().constLast().root, u"summit"_s);
    EXPECT_TRUE(waitFor([&] { return sent<toserver::AcceptChildren>().constLast().accept; })); // a parent, fast
    // The search that made it the parent reached us; the losing candidate is closed.
    ASSERT_EQ(searches.size(), 1);
    const auto search = searches.at(0).at(0).value<distrib::Search>();
    EXPECT_EQ(search.username, u"alice"_s);
    EXPECT_EQ(search.token, 7U);
    EXPECT_EQ(search.query, u"kel varo"_s);
    EXPECT_TRUE(waitFor([&] { return !quiet->isOpen(connections.at(0)); }));
    EXPECT_TRUE(chatty->isOpen(connections.at(1)));
    // Later offers are ignored while we have a parent.
    const auto late = makePeer(u"late"_s);
    server.sendTo(u"selecta"_s, fromserver::PossibleParents{{ParentCandidate{u"late"_s, kLoopback, late->port()}}});
    settle(200ms);
    EXPECT_EQ(late->connectionCount(), 0);
}

TEST_F(DistributedNodeTest, ARootCandidateNeedNotNameItsRoot)
{
    const auto top = makePeer(u"top"_s);
    const quint64 connection = offerParents({top.get()}).value(0);
    top->sendDistrib(connection, frame(distrib::BranchLevel{0}));
    top->sendDistrib(connection, searchFrame(searchPayload(u"bob"_s, 3, u"amber"_s)));
    ASSERT_TRUE(waitFor([&] { return node.hasParent(); }));
    EXPECT_EQ(node.branchLevel(), 1);
    EXPECT_EQ(node.branchRoot(), u"top"_s);
}

TEST_F(DistributedNodeTest, BadCandidatesAndStrangersAreClosed)
{
    const auto negative = makePeer(u"negative"_s);
    const quint64 connection = offerParents({negative.get()}).value(0);
    negative->sendDistrib(connection, frame(distrib::BranchLevel{-1}));
    EXPECT_TRUE(waitFor([&] { return !negative->isOpen(connection); }));
    EXPECT_FALSE(node.hasParent());
}

TEST_F(DistributedNodeTest, SearchesGoDownTheTreeAsTheBytesReceived)
{
    const auto parent = makePeer(u"parent"_s);
    const quint64 up = adopt(*parent, 1, u"top"_s);
    const auto child = makePeer(u"kid"_s);
    const quint64 down = join(*child);
    // The child hears our place first.
    ASSERT_TRUE(waitFor([&] { return !payloadsOf(*child, down, DistribCode::BranchRoot).isEmpty(); }));
    EXPECT_EQ(distrib::BranchLevel::decode(payloadsOf(*child, down, DistribCode::BranchLevel).value(0))->level, 2);
    EXPECT_EQ(distrib::BranchRoot::decode(payloadsOf(*child, down, DistribCode::BranchRoot).value(0))->root, u"top"_s);
    // A search with trailing bytes we don't understand travels on unchanged.
    const QSignalSpy searches(&node, &DistributedNode::searchRequestReceived);
    const QByteArray payload = searchPayload(u"carol"_s, 42, u"slow comet"_s) + QByteArray("\x01\x02future", 8);
    parent->sendDistrib(up, searchFrame(payload));
    ASSERT_TRUE(waitFor([&] { return payloadsOf(*child, down, DistribCode::Search).size() == 1; }));
    EXPECT_EQ(payloadsOf(*child, down, DistribCode::Search).at(0), payload);
    ASSERT_EQ(searches.size(), 1);
    EXPECT_EQ(searches.at(0).at(0).value<distrib::Search>().query, u"slow comet"_s);
    // The same search again is dropped; a new one goes on.
    parent->sendDistrib(up, searchFrame(payload));
    parent->sendDistrib(up, searchFrame(searchPayload(u"carol"_s, 43, u"amber engine"_s)));
    ASSERT_TRUE(waitFor([&] { return payloadsOf(*child, down, DistribCode::Search).size() == 2; }));
    settle(100ms);
    EXPECT_EQ(searches.size(), 2);
    // A wrapped search from the parent (older SoulseekQt roots) is unpacked the same way.
    parent->sendDistrib(up, frame(distrib::EmbeddedMessage{static_cast<quint8>(DistribCode::Search),
                                                           searchPayload(u"dave"_s, 44, u"glass signal"_s)}));
    ASSERT_TRUE(waitFor([&] { return searches.size() == 3; }));
    EXPECT_EQ(searches.at(2).at(0).value<distrib::Search>().username, u"dave"_s);
    EXPECT_TRUE(waitFor([&] { return payloadsOf(*child, down, DistribCode::Search).size() == 3; }));
    // Changes of place pass down too.
    parent->sendDistrib(up, frame(distrib::BranchLevel{4}));
    ASSERT_TRUE(waitFor([&] { return node.branchLevel() == 5; }));
    EXPECT_TRUE(waitFor([&] { return sent<toserver::BranchLevel>().constLast().level == 5; }));
    EXPECT_TRUE(waitFor([&] {
        const QList<QByteArray> levels = payloadsOf(*child, down, DistribCode::BranchLevel);
        return !levels.isEmpty() && distrib::BranchLevel::decode(levels.constLast())->level == 5;
    }));
}

TEST_F(DistributedNodeTest, AChildThatSendsSearchesIsClosed)
{
    const auto parent = makePeer(u"parent"_s);
    adopt(*parent, 1, u"top"_s);
    const auto child = makePeer(u"rude"_s);
    const quint64 down = join(*child);
    const QSignalSpy searches(&node, &DistributedNode::searchRequestReceived);
    child->sendDistrib(down, searchFrame(searchPayload(u"rude"_s, 1, u"anything"_s)));
    EXPECT_TRUE(waitFor([&] { return !child->isOpen(down); }));
    EXPECT_TRUE(node.childUsernames().isEmpty());
    EXPECT_TRUE(searches.isEmpty());
}

TEST_F(DistributedNodeTest, ChildrenOnlyWithRoomAndAParent)
{
    // Without a parent (nor being a root), nobody may join.
    const auto early = makePeer(u"early"_s);
    const quint64 refused =
        early->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Distributed);
    EXPECT_TRUE(waitFor([&] { return !early->isOpen(refused); }));
    // With one, up to the limit our speed allows: 20 kB/s at ratio 50 makes 4.
    const auto parent = makePeer(u"parent"_s);
    adopt(*parent, 0, u"parent"_s);
    server.sendTo(u"selecta"_s, fromserver::UserStatsUpdate{u"selecta"_s, UserStats{20'000, 0, 0, 0, 0}});
    ASSERT_TRUE(waitFor([&] { return node.maxChildren() == 4; }));
    std::vector<std::unique_ptr<FakePeer>> kids;
    for (int i = 0; i < 4; ++i) {
        kids.push_back(makePeer(u"kid%1"_s.arg(i)));
        join(*kids.back());
    }
    EXPECT_TRUE(waitFor([&] { return !sent<toserver::AcceptChildren>().constLast().accept; })); // full
    const auto extra = makePeer(u"extra"_s);
    const quint64 overflow =
        extra->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Distributed);
    EXPECT_TRUE(waitFor([&] { return !extra->isOpen(overflow); }));
    EXPECT_EQ(node.childUsernames().size(), 4);
    // Turning children off drops them.
    const QSignalSpy counts(&node, &DistributedNode::childrenChanged);
    node.setChildrenAllowed(false);
    EXPECT_TRUE(node.childUsernames().isEmpty());
    EXPECT_EQ(node.maxChildren(), 0);
    ASSERT_FALSE(counts.isEmpty());
    EXPECT_EQ(counts.constLast().at(0).toInt(), 0);
    EXPECT_TRUE(waitFor([&] { return kids.front()->connectionCount() == 0; }));
}

TEST_F(DistributedNodeTest, LosingTheParentKeepsTheChildren)
{
    const auto parent = makePeer(u"parent"_s);
    const quint64 up = adopt(*parent, 2, u"top"_s);
    const auto child = makePeer(u"kid"_s);
    const quint64 down = join(*child);
    const QSignalSpy parents(&node, &DistributedNode::parentChanged);
    parent->close(up);
    ASSERT_TRUE(waitFor([&] { return !node.hasParent(); }));
    ASSERT_EQ(parents.size(), 1);
    EXPECT_TRUE(parents.at(0).at(0).toString().isEmpty());
    EXPECT_EQ(node.branchLevel(), 0);
    EXPECT_EQ(node.branchRoot(), u"selecta"_s);
    EXPECT_TRUE(waitFor([&] { return sent<toserver::HaveNoParent>().constLast().noParent; }));
    EXPECT_TRUE(waitFor([&] { return sent<toserver::BranchLevel>().constLast().level == 0U; }));
    EXPECT_TRUE(waitFor([&] { return !sent<toserver::AcceptChildren>().constLast().accept; })); // nobody feeds us
    EXPECT_TRUE(child->isOpen(down));
    EXPECT_EQ(node.childUsernames(), QStringList{u"kid"_s});
    EXPECT_TRUE(waitFor([&] {
        const QList<QByteArray> levels = payloadsOf(*child, down, DistribCode::BranchLevel);
        return !levels.isEmpty() && distrib::BranchLevel::decode(levels.constLast())->level == 0;
    }));
}

TEST_F(DistributedNodeTest, ResetDropsParentAndChildren)
{
    const auto parent = makePeer(u"parent"_s);
    const quint64 up = adopt(*parent, 1, u"top"_s);
    const auto child = makePeer(u"kid"_s);
    const quint64 down = join(*child);
    server.sendTo(u"selecta"_s, fromserver::ResetDistributed{});
    ASSERT_TRUE(waitFor([&] { return !node.hasParent() && node.childUsernames().isEmpty(); }));
    EXPECT_TRUE(waitFor([&] { return !parent->isOpen(up) && !child->isOpen(down); }));
    EXPECT_TRUE(waitFor([&] { return sent<toserver::HaveNoParent>().constLast().noParent; }));
}

TEST_F(DistributedNodeTest, TheServerCanMakeUsABranchRoot)
{
    const QSignalSpy searches(&node, &DistributedNode::searchRequestReceived);
    server.sendTo(u"selecta"_s, fromserver::EmbeddedMessage{static_cast<quint8>(DistribCode::Search),
                                                            searchPayload(u"erin"_s, 5, u"copper ferry"_s)});
    ASSERT_TRUE(waitFor([&] { return searches.size() == 1; }));
    EXPECT_TRUE(node.isBranchRoot());
    EXPECT_EQ(node.branchLevel(), 0);
    EXPECT_EQ(searches.at(0).at(0).value<distrib::Search>().query, u"copper ferry"_s);
    // A root takes children and feeds them.
    EXPECT_TRUE(waitFor([&] { return sent<toserver::AcceptChildren>().constLast().accept; }));
    const auto child = makePeer(u"kid"_s);
    const quint64 down = join(*child);
    const QByteArray payload = searchPayload(u"erin"_s, 6, u"midnight ferry"_s);
    server.sendTo(u"selecta"_s, fromserver::EmbeddedMessage{static_cast<quint8>(DistribCode::Search), payload});
    ASSERT_TRUE(waitFor([&] { return payloadsOf(*child, down, DistribCode::Search).size() == 1; }));
    EXPECT_EQ(payloadsOf(*child, down, DistribCode::Search).at(0), payload); // unwrapped, as code 3
    // Anything but a search inside is ignored.
    server.sendTo(u"selecta"_s,
                  fromserver::EmbeddedMessage{static_cast<quint8>(DistribCode::BranchLevel), QByteArray(4, '\0')});
    settle(100ms);
    EXPECT_EQ(searches.size(), 2);
}
