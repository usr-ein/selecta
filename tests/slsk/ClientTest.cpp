// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Client.h"

#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/Samples.h"
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"

#include <QSignalSpy>

#include <gtest/gtest.h>
#include <memory>

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

class ClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        client = std::make_unique<Client>(permits);
        client->server().setTimeouts({2s, 2s, 5min});
        client->peers().setTimeouts({1s, 1s, 5s, 1s, 3s});
        ASSERT_TRUE(client->peers().listen(0, QHostAddress(QHostAddress::LocalHost)));
        client->server().setListenPort(client->peers().listenPort());
        ASSERT_TRUE(client->server().connectToServer(server.address(), server.port(), permit(Operation::Login)));
        ASSERT_TRUE(client->server().login(u"selecta"_s, u"pw"_s));
        ASSERT_TRUE(waitFor([&] { return client->server().isLoggedIn(); }));
        ASSERT_TRUE(waitFor([&] {
            const FakeServer::Client *me = server.client(u"selecta"_s);
            return me != nullptr && me->listenPort == client->peers().listenPort();
        }));
    }

    std::unique_ptr<FakePeer> makePeer(const QString &name)
    {
        auto peer = std::make_unique<FakePeer>(name, &server);
        EXPECT_TRUE(peer->start());
        return peer;
    }

    FakeServer server;
    TestPermits permits;
    std::unique_ptr<Client> client;
};

} // namespace

TEST_F(ClientTest, SearchOpensItsTokenAndStopSearchClosesIt)
{
    const auto alice = makePeer(u"alice"_s);
    alice->shares = selecta::slsk::testing::sampleShareList(2, 2);
    alice->searchHits = {u"album"_s};
    const QSignalSpy results(&client->peers(), &PeerNetwork::searchResponseReceived);
    const quint32 token = client->search(u"album"_s, permit(Operation::Search));
    ASSERT_NE(token, 0U);
    EXPECT_TRUE(client->peers().isSearchOpen(token));
    ASSERT_TRUE(waitFor([&] { return results.size() == 1; }));
    EXPECT_EQ(results.at(0).at(1).value<peer::FileSearchResponse>().token, token);
    const QList<toserver::FileSearch> searches = server.receivedOf<toserver::FileSearch>(u"selecta"_s);
    ASSERT_EQ(searches.size(), 1);
    EXPECT_EQ(searches.at(0).token, token);
    EXPECT_EQ(searches.at(0).query, u"album"_s);
    // Once stopped, late results are dropped.
    client->stopSearch(token);
    EXPECT_FALSE(client->peers().isSearchOpen(token));
    // Each search has its own token; user and wishlist searches too.
    const quint32 user = client->searchUser(u"alice"_s, u"music"_s, permit(Operation::Search));
    const quint32 wish = client->searchWishlist(u"music"_s, permit(Operation::Search));
    EXPECT_NE(user, 0U);
    EXPECT_NE(wish, 0U);
    EXPECT_NE(user, token);
    EXPECT_NE(wish, user);
    EXPECT_TRUE(waitFor([&] { return server.receivedOf<toserver::UserSearch>(u"selecta"_s).size() == 1; }));
    EXPECT_TRUE(waitFor([&] { return server.receivedOf<toserver::WishlistSearch>(u"selecta"_s).size() == 1; }));
}

TEST_F(ClientTest, SearchNeedsASessionAndItsPermit)
{
    EXPECT_EQ(client->search(u"music"_s, permit(Operation::Browse)), 0U);
    client->server().disconnectFromServer();
    EXPECT_EQ(client->search(u"music"_s, permit(Operation::Search)), 0U);
    settle();
}

TEST_F(ClientTest, SearchesFromBothRoutesBecomeOneSignal)
{
    const QSignalSpy incoming(client.get(), &Client::searchRequestReceived);
    // Server route (code 26): a user or room search, or any search on Soulfind.
    server.sendTo(u"selecta"_s, fromserver::SearchRequest{u"bob"_s, 11, u"amber engine"_s});
    ASSERT_TRUE(waitFor([&] { return incoming.size() == 1; }));
    EXPECT_EQ(incoming.at(0).at(0).value<IncomingSearch>(),
              (IncomingSearch{u"bob"_s, 11, u"amber engine"_s, IncomingSearch::Route::Server}));
    // Distributed route: through our parent.
    const auto parent = makePeer(u"parent"_s);
    const QSignalSpy identified(parent.get(), &FakePeer::identified);
    server.sendTo(u"selecta"_s, fromserver::PossibleParents{{ParentCandidate{u"parent"_s, kLoopback, parent->port()}}});
    ASSERT_TRUE(waitFor([&] { return !identified.isEmpty(); }));
    const quint64 connection = identified.at(0).at(0).toULongLong();
    parent->sendDistrib(connection, frame(distrib::BranchLevel{0}));
    parent->sendDistrib(connection,
                        frame(distrib::Search{protocol::kDistribSearchIdentifier, u"carol"_s, 12, u"slow comet"_s}));
    ASSERT_TRUE(waitFor([&] { return incoming.size() == 2; }));
    EXPECT_EQ(incoming.at(1).at(0).value<IncomingSearch>(),
              (IncomingSearch{u"carol"_s, 12, u"slow comet"_s, IncomingSearch::Route::Distributed}));
    EXPECT_EQ(client->distributed().parentUsername(), u"parent"_s);
}

TEST_F(ClientTest, AnswerSearchSendsOurResults)
{
    const auto bob = makePeer(u"bob"_s);
    const IncomingSearch search{u"bob"_s, 21, u"amber"_s, IncomingSearch::Route::Server};
    const QList<FileEntry> hits = {
        FileEntry{RemotePath::fromText(u"Music\\Kel Varo\\01 - Amber Engine.flac"_s), 12'345, u"flac"_s, {}}};
    EXPECT_FALSE(client->answerSearch(search, {}, {})); // nothing to say: nothing sent
    ASSERT_TRUE(client->answerSearch(search, hits, Client::UploadAvailability{false, 900'000, 3}));
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::FileSearchResponse>().size() == 1; }));
    const peer::FileSearchResponse response = bob->receivedOf<peer::FileSearchResponse>().at(0);
    EXPECT_EQ(response.username, u"selecta"_s);
    EXPECT_EQ(response.token, 21U);
    EXPECT_EQ(response.results, hits);
    EXPECT_FALSE(response.slotFree);
    EXPECT_EQ(response.averageSpeed, 900'000U);
    EXPECT_EQ(response.queueLength, 3U);
    EXPECT_EQ(permits.requests.constLast().second, ConnectPurpose::SearchResponse);
}

TEST_F(ClientTest, DestroysCleanlyWithEverythingRunning)
{
    const auto parent = makePeer(u"parent"_s);
    server.sendTo(u"selecta"_s, fromserver::PossibleParents{{ParentCandidate{u"parent"_s, kLoopback, parent->port()}}});
    client->search(u"anything"_s, permit(Operation::Search));
    settle(100ms);
    client.reset(); // components go in reverse: none may touch one already gone
    settle(100ms);
    EXPECT_TRUE(waitFor([&] { return parent->connectionCount() == 0; }));
}
