// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/PeerNetwork.h"

#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/Samples.h"
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"
#include "slsk/Providers.h"
#include "slsk/ServerSession.h"

#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpSocket>

#include <gtest/gtest.h>

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

class Shares : public ShareProvider
{
public:
    QByteArray sharedFileList(const QString &username) override
    {
        asked.append(username);
        return peer::SharedFileListResponse{list}.encode();
    }
    QList<SharedDirectory> folderContents(const QString &username, const RemotePath &folder) override
    {
        asked.append(username);
        QList<SharedDirectory> found;
        for (const SharedDirectory &directory : std::as_const(list.directories)) {
            if (directory.path == folder) {
                found.append(directory);
            }
        }
        return found;
    }
    SharedFileList list = selecta::slsk::testing::sampleShareList(2, 3);
    QStringList asked;
};

class Info : public UserInfoProvider
{
public:
    peer::UserInfoResponse userInfo(const QString & /*username*/) override { return info; }
    peer::UserInfoResponse info{u"Selecta test user"_s, std::nullopt, 3, 1, true, 0};
};

class PeerNetworkTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        session.setTimeouts({2s, 2s, 5min});
        peers.setTimeouts({1s, 400ms, 2s, 300ms, 1s});
        ASSERT_TRUE(peers.listen(0, QHostAddress(QHostAddress::LocalHost)));
        session.setListenPort(peers.listenPort());
        ASSERT_TRUE(session.connectToServer(server.address(), server.port(), permit(Operation::Login)));
        ASSERT_TRUE(session.login(u"selecta"_s, u"pw"_s));
        ASSERT_TRUE(waitFor([&] { return session.isLoggedIn(); }));
        // The server must know our listening port before peers look us up.
        ASSERT_TRUE(waitFor([&] {
            const FakeServer::Client *me = server.client(u"selecta"_s);
            return me != nullptr && me->listenPort == peers.listenPort();
        }));
    }

    // A peer the server knows: reachable directly unless `reachable` is false.
    std::unique_ptr<FakePeer> makePeer(const QString &name, bool reachable = true)
    {
        auto peer = std::make_unique<FakePeer>(name, &server);
        EXPECT_TRUE(peer->start(reachable));
        return peer;
    }

    FakeServer server;
    ServerSession session;
    TestPermits permits;
    PeerNetwork peers{session, permits};
};

} // namespace

// --- outbound connections --------------------------------------------------------------------------------------------

TEST_F(PeerNetworkTest, DirectConnectionCarriesABrowse)
{
    const auto alice = makePeer(u"alice"_s);
    alice->answerIndirect = false; // only the direct half of the race can win
    alice->shares = selecta::slsk::testing::sampleShareList(3, 2);
    const QSignalSpy lists(&peers, &PeerNetwork::sharedFileListReceived);
    ASSERT_TRUE(peers.requestSharedFileList(u"alice"_s, permit(Operation::Browse)));
    ASSERT_TRUE(waitFor([&] { return lists.size() == 1; }));
    EXPECT_EQ(lists.at(0).at(0).toString(), u"alice"_s);
    EXPECT_EQ(lists.at(0).at(1).value<SharedFileList>(), alice->shares);
    EXPECT_EQ(alice->peerInitsFrom, QStringList{u"selecta"_s});
    EXPECT_TRUE(alice->pierceTokens.isEmpty());
    ASSERT_EQ(permits.requests.size(), 1);
    EXPECT_EQ(permits.requests.at(0).second, ConnectPurpose::Request);
    // Both halves of the race went out: the address lookup and the indirect request.
    EXPECT_EQ(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).size(), 1);
    EXPECT_EQ(server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).size(), 1);
    EXPECT_NE(server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).at(0).token, 0U);
}

TEST_F(PeerNetworkTest, IndirectPathWinsWhenTheDirectOneIsRefused)
{
    const auto bob = makePeer(u"bob"_s, false);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    bob->userInfo = peer::UserInfoResponse{u"bob here"_s, std::nullopt, 2, 0, true, std::nullopt};
    ASSERT_TRUE(peers.requestUserInfo(u"bob"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_EQ(infos.at(0).at(1).value<peer::UserInfoResponse>().description, u"bob here"_s);
    EXPECT_EQ(bob->indirectRequests, 1);
    EXPECT_TRUE(bob->peerInitsFrom.isEmpty()); // we never reached it directly
}

TEST_F(PeerNetworkTest, IndirectOnlyWhenTheServerHasNoPortForThePeer)
{
    FakePeer carol(u"carol"_s, &server);
    ASSERT_TRUE(carol.start());
    server.addVirtualUser(u"carol"_s, QHostAddress::LocalHost, 0, &carol); // port 0: indirect only
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"carol"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_EQ(carol.indirectRequests, 1);
}

TEST_F(PeerNetworkTest, UnreachablePeerFailsAfterTheIndirectWait)
{
    const auto dave = makePeer(u"dave"_s, false);
    dave->answerIndirect = false;
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestSharedFileList(u"dave"_s, permit(Operation::Browse)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(1).value<PeerRequest>(), PeerRequest::SharedFileList);
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::Unreachable);
    // A later request starts over.
    ASSERT_TRUE(peers.requestSharedFileList(u"dave"_s, permit(Operation::Browse)));
    EXPECT_TRUE(waitFor([&] { return permits.requestsFor(u"dave"_s) == 2; }));
}

TEST_F(PeerNetworkTest, CantConnectToPeerEndsTheWaitEarly)
{
    peers.setTimeouts({1s, 10s, 20s, 300ms, 10s}); // a long indirect wait that must not be needed
    const auto erin = makePeer(u"erin"_s, false);
    erin->answerIndirect = false;
    erin->reportCantConnect = true;
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"erin"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }, 3s));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::Unreachable);
}

TEST_F(PeerNetworkTest, OfflineUserFailsAtOnce)
{
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"nobody"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::UserOffline);
}

TEST_F(PeerNetworkTest, RefusedPermitSendsNothing)
{
    permits.mode = TestPermits::Mode::Refuse;
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestSharedFileList(u"alice"_s, permit(Operation::Browse)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::NotPermitted);
    settle();
    EXPECT_TRUE(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).isEmpty());
    EXPECT_TRUE(server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).isEmpty());
    EXPECT_EQ(alice->connectionCount(), 0);
}

TEST_F(PeerNetworkTest, HeldPermitHoldsTheConnection)
{
    permits.mode = TestPermits::Mode::Defer;
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    settle();
    EXPECT_EQ(permits.held(), 1);
    EXPECT_TRUE(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).isEmpty());
    permits.release();
    EXPECT_TRUE(waitFor([&] { return infos.size() == 1; }));
}

TEST_F(PeerNetworkTest, SynchronousPermitWorksToo)
{
    permits.synchronous = true;
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    EXPECT_TRUE(waitFor([&] { return infos.size() == 1; }));
}

TEST_F(PeerNetworkTest, APeerAtOurOwnAddressIsReachedOverLoopback)
{
    // A local server hands out this machine's LAN address for clients on 127.0.0.1 (Soulfind does); the peer listens
    // on 127.0.0.1 only, so only loopback reaches it.
    QHostAddress lan;
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
            lan = address;
            break;
        }
    }
    if (lan.isNull()) {
        GTEST_SKIP() << "no LAN address on this machine";
    }
    FakePeer carol(u"carol"_s, &server);
    carol.answerIndirect = false;
    ASSERT_TRUE(carol.start());
    server.addVirtualUser(u"carol"_s, lan, carol.port(), &carol);
    carol.userInfo = peer::UserInfoResponse{u"same machine"_s, std::nullopt, 1, 0, true, std::nullopt};
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"carol"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_EQ(infos.at(0).at(1).value<peer::UserInfoResponse>().description, u"same machine"_s);
}

TEST_F(PeerNetworkTest, RequestsToOnePeerShareOneConnectionAndOneLookup)
{
    const auto alice = makePeer(u"alice"_s);
    alice->answerIndirect = false; // a dial-back that loses the race would add a second, read-only connection
    alice->shares = selecta::slsk::testing::sampleShareList(2, 2);
    const QSignalSpy lists(&peers, &PeerNetwork::sharedFileListReceived);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    const QSignalSpy folders(&peers, &PeerNetwork::folderContentsReceived);
    ASSERT_TRUE(peers.requestSharedFileList(u"alice"_s, permit(Operation::Browse)));
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    const quint32 token = peers.requestFolderContents(u"alice"_s, alice->shares.directories.at(1).path,
                                                      permit(Operation::FolderContents));
    ASSERT_NE(token, 0U);
    ASSERT_TRUE(waitFor([&] { return lists.size() == 1 && infos.size() == 1 && folders.size() == 1; }));
    const auto contents = folders.at(0).at(1).value<peer::FolderContentsResponse>();
    EXPECT_EQ(contents.token, token);
    ASSERT_EQ(contents.directories.size(), 1);
    EXPECT_EQ(contents.directories.at(0), alice->shares.directories.at(1));
    EXPECT_EQ(permits.requestsFor(u"alice"_s), 1);
    EXPECT_EQ(alice->connectionCount(), 1);
    // Later requests reuse the connection: no new lookup.
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 2; }));
    EXPECT_EQ(permits.requestsFor(u"alice"_s), 1);
    EXPECT_TRUE(peers.hasPeerConnection(u"alice"_s));
}

TEST_F(PeerNetworkTest, RequestsNeedTheirOwnPermit)
{
    EXPECT_FALSE(peers.requestSharedFileList(u"alice"_s, permit(Operation::UserInfo)));
    EXPECT_FALSE(peers.requestUserInfo(u"alice"_s, permit(Operation::Browse)));
    EXPECT_EQ(peers.requestFolderContents(u"alice"_s, RemotePath::fromText(u"x"_s), permit(Operation::Browse)), 0U);
    EXPECT_FALSE(peers.requestSharedFileList(QString(), permit(Operation::Browse)));
    settle();
    EXPECT_TRUE(permits.requests.isEmpty());
}

TEST_F(PeerNetworkTest, PeerThatNeverAnswersTimesOut)
{
    const auto alice = makePeer(u"alice"_s);
    alice->ignoreRequests = true;
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    const quint32 token =
        peers.requestFolderContents(u"alice"_s, RemotePath::fromText(u"Music"_s), permit(Operation::FolderContents));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(1).value<PeerRequest>(), PeerRequest::FolderContents);
    EXPECT_EQ(failed.at(0).at(2).toUInt(), token);
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::Timeout);
}

TEST_F(PeerNetworkTest, ClosedConnectionFailsWrittenRequests)
{
    const auto alice = makePeer(u"alice"_s);
    alice->answerIndirect = false; // one connection: closing it leaves none to answer on
    alice->ignoreRequests = true;
    alice->onMessage = [&alice](quint64 connection, const QString &, const PeerMessage &) { alice->close(connection); };
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::ConnectionClosed);
}

TEST_F(PeerNetworkTest, NothingWithoutAServerSession)
{
    session.disconnectFromServer();
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::NotLoggedIn);
}

TEST_F(PeerNetworkTest, SessionEndFailsAttemptsStillWaiting)
{
    permits.mode = TestPermits::Mode::Defer;
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    session.disconnectFromServer();
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::NotLoggedIn);
    permits.release(); // a late permit for a request given up: dropped
    settle();
    EXPECT_EQ(failed.size(), 1);
}

TEST_F(PeerNetworkTest, CloseAllCancelsPendingWork)
{
    permits.mode = TestPermits::Mode::Defer;
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(peers.requestSharedFileList(u"bob"_s, permit(Operation::Browse)));
    peers.closeAll();
    ASSERT_EQ(failed.size(), 2);
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::Cancelled);
    EXPECT_EQ(failed.at(1).at(3).value<PeerError>(), PeerError::Cancelled);
}

TEST_F(PeerNetworkTest, BothHalvesOfTheRaceSucceedingIsHarmless)
{
    // The peer gets our PeerInit and also dials back for our ConnectToPeer: the late connection is kept for reading
    // and nothing is lost or duplicated.
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
        ASSERT_TRUE(waitFor([&] { return infos.size() == i + 1; }));
    }
    settle(200ms);
    EXPECT_EQ(infos.size(), 3);
    EXPECT_EQ(alice->receivedOf<peer::UserInfoRequest>().size(), 3);
    EXPECT_EQ(permits.requestsFor(u"alice"_s), 1);
}

TEST_F(PeerNetworkTest, IndirectTokensAreUniqueAndNeverZero)
{
    permits.mode = TestPermits::Mode::Grant;
    for (const QString &name : {u"u1"_s, u"u2"_s, u"u3"_s}) {
        ASSERT_TRUE(peers.requestUserInfo(name, permit(Operation::UserInfo)));
    }
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s).size() == 3; }));
    QSet<quint32> tokens;
    const QList<toserver::ConnectToPeer> requests = server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s);
    for (const toserver::ConnectToPeer &request : requests) {
        EXPECT_NE(request.token, 0U);
        tokens.insert(request.token);
    }
    EXPECT_EQ(tokens.size(), 3);
}

// --- inbound connections ---------------------------------------------------------------------------------------------

TEST_F(PeerNetworkTest, AnswersFromTheProviders)
{
    Shares shares;
    Info info;
    peers.setShareProvider(&shares);
    peers.setUserInfoProvider(&info);
    const QSignalSpy browseAsked(&peers, &PeerNetwork::sharedFileListRequested);
    const QSignalSpy folderAsked(&peers, &PeerNetwork::folderContentsRequested);
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c, frame(peer::GetShareFileList{}));
    alice->send(c, frame(peer::UserInfoRequest{}));
    alice->send(c, frame(peer::FolderContentsRequest{9, shares.list.directories.at(0).path}));
    alice->send(c, frame(peer::FolderContentsRequest{10, RemotePath::fromText(u"not shared"_s)}));
    ASSERT_TRUE(waitFor([&] { return alice->received.size() == 4; }));
    EXPECT_EQ(alice->receivedOf<peer::SharedFileListResponse>().at(0).list, shares.list);
    EXPECT_EQ(alice->receivedOf<peer::UserInfoResponse>().at(0), info.info);
    const QList<peer::FolderContentsResponse> folders = alice->receivedOf<peer::FolderContentsResponse>();
    ASSERT_EQ(folders.size(), 2);
    EXPECT_EQ(folders.at(0).directories.size(), 1);
    EXPECT_TRUE(folders.at(1).directories.isEmpty()); // unknown folder: an empty listing
    EXPECT_EQ(browseAsked.size(), 1);
    EXPECT_EQ(folderAsked.size(), 2);
    EXPECT_TRUE(shares.asked.contains(u"alice"_s));
    peers.setShareProvider(nullptr);
    peers.setUserInfoProvider(nullptr);
}

TEST_F(PeerNetworkTest, WithoutProvidersAnswersAreEmpty)
{
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c, frame(peer::GetShareFileList{}));
    alice->send(c, frame(peer::UserInfoRequest{}));
    ASSERT_TRUE(waitFor([&] { return alice->received.size() == 2; }));
    EXPECT_EQ(alice->receivedOf<peer::SharedFileListResponse>().at(0).list, SharedFileList{});
    EXPECT_EQ(alice->receivedOf<peer::UserInfoResponse>().at(0), peer::UserInfoResponse{});
}

TEST_F(PeerNetworkTest, InboundConnectionIsReusedForOurRequests)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    ASSERT_TRUE(waitFor([&] { return peers.connectionCount(ConnectionType::Peer) == 1; }));
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_TRUE(permits.requests.isEmpty()); // no lookup: the peer's own connection carried it
    EXPECT_EQ(alice->connectionCount(), 1);
}

TEST_F(PeerNetworkTest, SearchResultsOnlyForOpenSearches)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy results(&peers, &PeerNetwork::searchResponseReceived);
    peers.openSearch(77);
    peer::FileSearchResponse response;
    response.username = u"alice"_s;
    response.results.append(selecta::slsk::testing::sampleFile(0));
    response.token = 78; // not ours
    const quint64 first =
        alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(first, frame(response));
    response.token = 77;
    const quint64 second =
        alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(second, frame(response));
    ASSERT_TRUE(waitFor([&] { return results.size() == 1; }));
    EXPECT_EQ(results.at(0).at(0).toString(), u"alice"_s);
    EXPECT_EQ(results.at(0).at(1).value<peer::FileSearchResponse>().token, 77U);
    // The peer opened these connections just to deliver results: we close them once read.
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(second); }));
    peers.closeSearch(77);
    EXPECT_FALSE(peers.isSearchOpen(77));
    const quint64 third =
        alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(third, frame(response));
    settle(200ms);
    EXPECT_EQ(results.size(), 1);
}

TEST_F(PeerNetworkTest, OurSearchAnswerOpensAShortLivedConnection)
{
    const auto alice = makePeer(u"alice"_s);
    peer::FileSearchResponse response;
    response.username = u"selecta"_s;
    response.token = 4242;
    response.results.append(selecta::slsk::testing::sampleFile(1));
    std::optional<PeerError> sent;
    peers.sendSearchResponse(u"alice"_s, response, [&sent](PeerError error) { sent = error; });
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::FileSearchResponse>().size() == 1; }));
    EXPECT_EQ(sent, PeerError::None);
    EXPECT_EQ(alice->receivedOf<peer::FileSearchResponse>().at(0).token, 4242U);
    ASSERT_EQ(permits.requests.size(), 1);
    EXPECT_EQ(permits.requests.at(0).second, ConnectPurpose::SearchResponse);
    // Closed once written.
    EXPECT_TRUE(waitFor([&] { return alice->connectionCount() == 0; }));
}

TEST_F(PeerNetworkTest, UnsolicitedShareListClosesTheConnection)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy lists(&peers, &PeerNetwork::sharedFileListReceived);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c, frame(peer::SharedFileListResponse{selecta::slsk::testing::sampleShareList(1, 1)}));
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(c); }));
    EXPECT_TRUE(lists.isEmpty());
}

TEST_F(PeerNetworkTest, UnsolicitedUserInfoClosesTheConnection)
{
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c, frame(peer::UserInfoResponse{u"spam"_s, std::nullopt, 0, 0, false, std::nullopt}));
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(c); }));
}

TEST_F(PeerNetworkTest, OversizeFrameClosesTheConnection)
{
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    // A QueueUpload announcing 2 MiB: over the 1 MiB cap for ordinary peer messages.
    alice->send(c, QByteArray::fromHex("04 00 20 00 2b 00 00 00"));
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(c); }));
}

TEST_F(PeerNetworkTest, MalformedAndUnknownMessagesAreSkipped)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy queued(&peers, &PeerNetwork::queueUploadReceived);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c, encodeFrame(CodeWidth::Word, 43, QByteArray::fromHex("ff ff ff ff"))); // malformed QueueUpload
    alice->send(c, encodeFrame(CodeWidth::Word, 999, "unknown"));
    alice->send(c, encodeFrame(CodeWidth::Word, 47, "obsolete"));
    alice->send(c, frame(peer::QueueUpload{RemotePath::fromText(u"Music\\a.mp3"_s)}));
    ASSERT_TRUE(waitFor([&] { return queued.size() == 1; }));
    EXPECT_TRUE(alice->isOpen(c));
}

TEST_F(PeerNetworkTest, TransferMessagesBecomeSignals)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy requests(&peers, &PeerNetwork::transferRequestReceived);
    const QSignalSpy responses(&peers, &PeerNetwork::transferResponseReceived);
    const QSignalSpy places(&peers, &PeerNetwork::placeInQueueRequestReceived);
    const QSignalSpy placeAnswers(&peers, &PeerNetwork::placeInQueueResponseReceived);
    const QSignalSpy failures(&peers, &PeerNetwork::uploadFailedReceived);
    const QSignalSpy denials(&peers, &PeerNetwork::uploadDeniedReceived);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    const RemotePath path = RemotePath::fromText(u"x.flac"_s);
    alice->send(c, frame(peer::TransferRequest{TransferDirection::Upload, 1, path, 10}));
    alice->send(c, frame(peer::TransferResponse{2, true, std::nullopt, {}}));
    alice->send(c, frame(peer::PlaceInQueueRequest{path}));
    alice->send(c, frame(peer::PlaceInQueueResponse{path, 3}));
    alice->send(c, frame(peer::UploadFailed{path}));
    alice->send(c, frame(peer::UploadDenied{path, u"Banned"_s}));
    ASSERT_TRUE(waitFor([&] { return denials.size() == 1; }));
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(responses.size(), 1);
    EXPECT_EQ(places.size(), 1);
    EXPECT_EQ(placeAnswers.size(), 1);
    EXPECT_EQ(failures.size(), 1);
    EXPECT_EQ(denials.at(0).at(0).toString(), u"alice"_s);
}

TEST_F(PeerNetworkTest, UnknownPierceTokenIsRefused)
{
    QTcpSocket raw;
    raw.connectToHost(QHostAddress::LocalHost, peers.listenPort());
    ASSERT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::ConnectedState; }));
    raw.write(frame(peerinit::PierceFireWall{123456}));
    EXPECT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::UnconnectedState; }));
}

TEST_F(PeerNetworkTest, UnacceptableUsernameIsRefused)
{
    for (const QString &name : {u"server"_s, u"bad\nname"_s}) {
        QTcpSocket raw;
        raw.connectToHost(QHostAddress::LocalHost, peers.listenPort());
        ASSERT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::ConnectedState; }));
        raw.write(frame(peerinit::PeerInit{name, ConnectionType::Peer, 0}));
        EXPECT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::UnconnectedState; }));
    }
}

TEST_F(PeerNetworkTest, SilentInboundSocketIsDropped)
{
    QTcpSocket raw;
    raw.connectToHost(QHostAddress::LocalHost, peers.listenPort());
    ASSERT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::ConnectedState; }));
    EXPECT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::UnconnectedState; }, 2s));
}

TEST_F(PeerNetworkTest, IdleConnectionsClose)
{
    const auto alice = makePeer(u"alice"_s);
    alice->answerIndirect = false;
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_EQ(peers.connectionCount(ConnectionType::Peer), 1);
    EXPECT_TRUE(waitFor([&] { return peers.connectionCount(ConnectionType::Peer) == 0; }, 4s));
    EXPECT_TRUE(waitFor([&] { return alice->connectionCount() == 0; }));
}

TEST_F(PeerNetworkTest, SocketLimitRefusesNewConnections)
{
    peers.setSocketLimit(1);
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    ASSERT_TRUE(waitFor([&] { return peers.socketCount() == 1; }));
    // A second inbound socket is refused.
    const quint64 extra =
        alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(extra); }));
    EXPECT_TRUE(alice->isOpen(c));
    // A new outbound attempt fails without asking for a permit, on the next turn: never inside the call.
    const QSignalSpy failed(&peers, &PeerNetwork::requestFailed);
    ASSERT_TRUE(peers.requestUserInfo(u"bob"_s, permit(Operation::UserInfo)));
    EXPECT_TRUE(failed.isEmpty());
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(3).value<PeerError>(), PeerError::SocketLimit);
    EXPECT_EQ(permits.requestsFor(u"bob"_s), 0);
}

// --- dial-backs ------------------------------------------------------------------------------------------------------

TEST_F(PeerNetworkTest, DialsBackForARelayedPeerConnection)
{
    const auto alice = makePeer(u"alice"_s);
    ASSERT_TRUE(alice->requestIndirect(u"selecta"_s, ConnectionType::Peer, 31337));
    ASSERT_TRUE(waitFor([&] { return alice->pierceTokens.contains(31337U); }));
    // That connection is now ours to use: no lookup for the next request.
    alice->userInfo = peer::UserInfoResponse{u"via dial-back"_s, std::nullopt, 1, 0, true, std::nullopt};
    const QSignalSpy infos(&peers, &PeerNetwork::userInfoReceived);
    ASSERT_TRUE(peers.requestUserInfo(u"alice"_s, permit(Operation::UserInfo)));
    ASSERT_TRUE(waitFor([&] { return infos.size() == 1; }));
    EXPECT_TRUE(permits.requests.isEmpty());
}

TEST_F(PeerNetworkTest, FailedDialBackIsReported)
{
    const auto ghost = makePeer(u"ghost"_s, false); // the server hands out a closed port
    ASSERT_TRUE(ghost->requestIndirect(u"selecta"_s, ConnectionType::Peer, 777));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::CantConnectToPeer>(u"selecta"_s).size() == 1; }));
    const toserver::CantConnectToPeer report = server.receivedOf<toserver::CantConnectToPeer>(u"selecta"_s).at(0);
    EXPECT_EQ(report.token, 777U);
    EXPECT_EQ(report.username, u"ghost"_s);
}

TEST_F(PeerNetworkTest, FileDialBackOnlyForExpectedTransfers)
{
    const auto alice = makePeer(u"alice"_s);
    ASSERT_TRUE(alice->requestIndirect(u"selecta"_s, ConnectionType::File, 41));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::CantConnectToPeer>(u"selecta"_s).size() == 1; }));
    EXPECT_FALSE(alice->pierceTokens.contains(41U));

    const QSignalSpy taken(&peers, &PeerNetwork::fileConnectionReady);
    peers.setFileConnectionFilter([](const QString &user) { return user == u"alice"_s; });
    ASSERT_TRUE(alice->requestIndirect(u"selecta"_s, ConnectionType::File, 42));
    ASSERT_TRUE(waitFor([&] { return taken.size() == 1; }));
    EXPECT_EQ(taken.at(0).at(0).toULongLong(), 0U); // not one of our requests: the peer asked
    EXPECT_EQ(taken.at(0).at(1).toString(), u"alice"_s);
    EXPECT_TRUE(waitFor([&] { return alice->pierceTokens.contains(42U); }));
    EXPECT_EQ(peers.socketCount(), 1); // handed off but still counted
    delete taken.at(0).at(2).value<QTcpSocket *>();
    EXPECT_EQ(peers.socketCount(), 0);
}

TEST_F(PeerNetworkTest, DistributedDialBackOnlyWhenWanted)
{
    const auto alice = makePeer(u"alice"_s);
    ASSERT_TRUE(alice->requestIndirect(u"selecta"_s, ConnectionType::Distributed, 51));
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::CantConnectToPeer>(u"selecta"_s).size() == 1; }));
    const QSignalSpy connected(&peers, &PeerNetwork::distributedConnected);
    peers.setDistributedConnectionFilter([](const QString &) { return true; });
    ASSERT_TRUE(alice->requestIndirect(u"selecta"_s, ConnectionType::Distributed, 52));
    ASSERT_TRUE(waitFor([&] { return connected.size() == 1; }));
    EXPECT_EQ(connected.at(0).at(0).toString(), u"alice"_s);
    EXPECT_FALSE(connected.at(0).at(1).toBool()); // the peer asked: not a parent candidate of ours
}

// --- F and D connections ---------------------------------------------------------------------------------------------

TEST_F(PeerNetworkTest, OpenFileConnectionHandsTheSocketOver)
{
    const auto alice = makePeer(u"alice"_s);
    alice->answerIndirect = false;
    const QSignalSpy taken(&peers, &PeerNetwork::fileConnectionReady);
    const quint64 id = peers.openFileConnection(u"alice"_s);
    ASSERT_TRUE(waitFor([&] { return taken.size() == 1; }));
    EXPECT_EQ(taken.at(0).at(0).toULongLong(), id);
    EXPECT_TRUE(waitFor([&] { return alice->peerInitsFrom.contains(u"selecta"_s); }));
    ASSERT_EQ(permits.requests.size(), 1);
    EXPECT_EQ(permits.requests.at(0).second, ConnectPurpose::Transfer);
    delete taken.at(0).at(2).value<QTcpSocket *>();
}

TEST_F(PeerNetworkTest, FileConnectionOverTheIndirectPath)
{
    const auto bob = makePeer(u"bob"_s, false);
    const QSignalSpy taken(&peers, &PeerNetwork::fileConnectionReady);
    const quint64 id = peers.openFileConnection(u"bob"_s);
    ASSERT_TRUE(waitFor([&] { return taken.size() == 1; }));
    EXPECT_EQ(taken.at(0).at(0).toULongLong(), id); // the dial-back serves our request
    EXPECT_EQ(bob->indirectRequests, 1);
    delete taken.at(0).at(2).value<QTcpSocket *>();
}

TEST_F(PeerNetworkTest, FileConnectionFailuresAreReported)
{
    const auto dave = makePeer(u"dave"_s, false);
    dave->answerIndirect = false;
    const QSignalSpy failed(&peers, &PeerNetwork::fileConnectionFailed);
    const quint64 id = peers.openFileConnection(u"dave"_s);
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(0).toULongLong(), id);
    EXPECT_EQ(failed.at(0).at(2).value<PeerError>(), PeerError::Unreachable);

    // A cancelled one is never heard of again, even when its permit comes later.
    permits.mode = TestPermits::Mode::Defer;
    const QSignalSpy taken(&peers, &PeerNetwork::fileConnectionReady);
    const quint64 cancelled = peers.openFileConnection(u"dave"_s);
    peers.cancelFileConnection(cancelled);
    permits.release();
    settle();
    EXPECT_EQ(failed.size(), 1);
    EXPECT_TRUE(taken.isEmpty());
    EXPECT_EQ(peers.socketCount(), 0);
}

TEST_F(PeerNetworkTest, FileConnectionFailuresComeAfterTheCall)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy failed(&peers, &PeerNetwork::fileConnectionFailed);
    // A permit refused on the spot.
    permits.mode = TestPermits::Mode::Refuse;
    permits.synchronous = true;
    const quint64 refused = peers.openFileConnection(u"alice"_s);
    EXPECT_TRUE(failed.isEmpty());
    ASSERT_TRUE(waitFor([&] { return failed.size() == 1; }));
    EXPECT_EQ(failed.at(0).at(0).toULongLong(), refused);
    EXPECT_EQ(failed.at(0).at(2).value<PeerError>(), PeerError::NotPermitted);
    // No session: cancelling before the next turn silences it.
    session.disconnectFromServer();
    const quint64 silenced = peers.openFileConnection(u"alice"_s);
    peers.cancelFileConnection(silenced);
    const quint64 reported = peers.openFileConnection(u"alice"_s);
    EXPECT_EQ(failed.size(), 1);
    ASSERT_TRUE(waitFor([&] { return failed.size() == 2; }));
    EXPECT_EQ(failed.at(1).at(0).toULongLong(), reported);
    EXPECT_EQ(failed.at(1).at(2).value<PeerError>(), PeerError::NotLoggedIn);
    settle();
    EXPECT_EQ(failed.size(), 2);
}

TEST_F(PeerNetworkTest, DistributedConnectionToACandidate)
{
    const auto parent = makePeer(u"parent"_s);
    const QSignalSpy identified(parent.get(), &FakePeer::identified);
    const QSignalSpy connected(&peers, &PeerNetwork::distributedConnected);
    const QSignalSpy messages(&peers, &PeerNetwork::distributedMessageReceived);
    const QSignalSpy disconnected(&peers, &PeerNetwork::distributedDisconnected);
    peers.openDistributedConnection(u"parent"_s, QHostAddress::LocalHost, parent->port());
    ASSERT_TRUE(waitFor([&] { return connected.size() == 1; }));
    EXPECT_TRUE(connected.at(0).at(1).toBool());
    EXPECT_EQ(permits.requests.at(0).second, ConnectPurpose::Distributed);
    // The candidate's address was known: only ConnectToPeer, no GetPeerAddress.
    EXPECT_TRUE(server.receivedOf<toserver::GetPeerAddress>(u"selecta"_s).isEmpty());
    ASSERT_TRUE(waitFor([&] { return identified.size() == 1; }));
    EXPECT_EQ(identified.at(0).at(2).value<ConnectionType>(), ConnectionType::Distributed);
    parent->sendDistrib(identified.at(0).at(0).toULongLong(), frame(distrib::BranchLevel{2}));
    ASSERT_TRUE(waitFor([&] { return messages.size() == 1; }));
    EXPECT_EQ(messages.at(0).at(1).toUInt(), static_cast<quint32>(DistribCode::BranchLevel));
    peers.sendDistributedMessage(u"parent"_s, frame(distrib::BranchRoot{u"me"_s}));
    ASSERT_TRUE(waitFor([&] { return parent->distribReceived.size() == 1; }));
    peers.closeDistributedConnection(u"parent"_s);
    EXPECT_EQ(disconnected.size(), 1);
}

TEST_F(PeerNetworkTest, OversizeDistributedFrameClosesTheConnection)
{
    const auto child = makePeer(u"child"_s);
    const QSignalSpy connected(&peers, &PeerNetwork::distributedConnected);
    const QSignalSpy disconnected(&peers, &PeerNetwork::distributedDisconnected);
    const quint64 c =
        child->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Distributed);
    ASSERT_TRUE(waitFor([&] { return connected.size() == 1; }));
    EXPECT_FALSE(connected.at(0).at(1).toBool());
    child->send(c, encodeFrame(CodeWidth::Byte, 3, QByteArray(20'000, 'x'))); // over 16 KiB
    EXPECT_TRUE(waitFor([&] { return disconnected.size() == 1; }));
}

// --- big share lists -------------------------------------------------------------------------------------------------

TEST_F(PeerNetworkTest, BigShareListArrivesWithProgress)
{
    const auto alice = makePeer(u"alice"_s);
    // Enough distinct names to compress past the worker threshold (1 MiB).
    SharedFileList big;
    QRandomGenerator random(1);
    for (int d = 0; d < 500; ++d) {
        SharedDirectory directory;
        directory.path = RemotePath::fromText(u"Collection\\Folder %1 %2"_s.arg(d).arg(random.generate()));
        for (int f = 0; f < 100; ++f) {
            FileEntry file;
            file.name = RemotePath::fromText(u"%1 %2.flac"_s.arg(random.generate()).arg(random.generate64()));
            file.size = random.generate64();
            file.attributes.duration = random.bounded(1000U);
            directory.files.append(file);
        }
        big.directories.append(directory);
    }
    alice->shares = big;
    ASSERT_GT(peer::SharedFileListResponse{big}.encode().size(), protocol::kMiB);
    const QSignalSpy lists(&peers, &PeerNetwork::sharedFileListReceived);
    const QSignalSpy progress(&peers, &PeerNetwork::sharedFileListProgress);
    ASSERT_TRUE(peers.requestSharedFileList(u"alice"_s, permit(Operation::Browse)));
    ASSERT_TRUE(waitFor([&] { return lists.size() == 1; }, 20s));
    EXPECT_EQ(lists.at(0).at(1).value<SharedFileList>(), big);
    EXPECT_FALSE(progress.isEmpty());
}
