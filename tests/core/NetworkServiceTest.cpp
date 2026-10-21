// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetworkService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/FakeRouter.h"
#include "fixtures/FakeServer.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "meta/TokenStore.h"
#include "slsk/Frame.h"
#include "slsk/PeerMessages.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "support/OpenFiles.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThreadPool>

#include <atomic>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::NetworkConfig;
using selecta::core::NetworkService;
using selecta::core::NetworkStatus;
using State = NetworkStatus::State;

namespace {

class NetworkServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        service = std::make_unique<NetworkService>(governor, log, tokens, [] { return qint64{1'800'000'000'000}; });
    }

    static NetworkConfig config(const QString &host, quint16 port)
    {
        NetworkConfig result;
        result.host = host;
        result.port = port;
        result.username = u"selecta_test"_s;
        result.listenPort = 0; // no listening socket in unit tests
        return result;
    }

    template <typename Condition>
    static bool settle(Condition done)
    {
        return QTest::qWaitFor(done, 3000);
    }

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    std::unique_ptr<NetworkService> service;
};

} // namespace

TEST_F(NetworkServiceTest, NoAccountMeansNotConfigured)
{
    NetworkConfig empty = config(u"127.0.0.1"_s, 2242);
    empty.username.clear();
    service->start(empty);
    EXPECT_EQ(service->status().state, State::NotConfigured);
}

TEST_F(NetworkServiceTest, NoPasswordMeansNotConfigured)
{
    service->start(config(u"127.0.0.1"_s, 2242));
    ASSERT_TRUE(settle([this] { return service->status().state == State::NotConfigured; }));
}

TEST_F(NetworkServiceTest, TheLiveServerIsRefusedBeforeAnyLookup)
{
    service->start(config(u"server.slsknet.org"_s, 2242));
    service->setPassword(u"not-a-real-password"_s);
    ASSERT_TRUE(settle([this] { return service->status().state == State::NeedsUser; }));
    EXPECT_FALSE(service->status().detail.isEmpty());
    bool logged = false;
    const auto lines = selecta::core::ActivityLog::recent(db, 20);
    for (const auto &line : lines) {
        logged = logged || line.outcome == u"live-server-refused"_s;
    }
    EXPECT_TRUE(logged);
}

TEST_F(NetworkServiceTest, APasswordIsKeptInTheTokenStore)
{
    service->start(config(u"127.0.0.1"_s, 1));
    service->setPassword(u"s3cret"_s);
    ASSERT_TRUE(settle([this] { return tokens.peek(NetworkService::passwordAccount(u"selecta_test"_s)).has_value(); }));
    EXPECT_EQ(*tokens.peek(NetworkService::passwordAccount(u"selecta_test"_s)), QByteArray("s3cret"));
    service->disconnectNow();
}

TEST_F(NetworkServiceTest, AnUnreachableServerSchedulesARetry)
{
    // Nothing listens on port 1: the connection is refused at once and the reconnect policy takes over.
    service->start(config(u"127.0.0.1"_s, 1));
    service->setPassword(u"pw"_s); // typed before the stored (empty) one was read: the typed one wins
    ASSERT_TRUE(settle([this] { return service->status().state == State::Reconnecting; }));
    EXPECT_GT(service->status().retryAtMs, qint64{1'800'000'000'000});
    service->disconnectNow();
    EXPECT_EQ(service->status().state, State::Offline);
}

TEST_F(NetworkServiceTest, PeerLookupPermitsComeOnALaterTurn)
{
    std::optional<selecta::governor::Permit> received;
    bool called = false;
    service->permits().requestPeerLookup(u"vinyl_ghost"_s, selecta::slsk::ConnectPurpose::Request,
                                         [&](std::optional<selecta::governor::Permit> permit) {
                                             called = true;
                                             received = std::move(permit);
                                         });
    EXPECT_FALSE(called);
    ASSERT_TRUE(settle([&called] { return called; }));
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->operation(), selecta::governor::Operation::PeerLookup);
}

TEST_F(NetworkServiceTest, PeerLookupsAreRefusedWhenTheBudgetIsSpent)
{
    int refused = 0;
    int granted = 0;
    const int asks = selecta::governor::limits::kPeerLookupsInFlight + 5;
    for (int i = 0; i < asks; ++i) {
        service->permits().requestPeerLookup(
            u"peer%1"_s.arg(i), selecta::slsk::ConnectPurpose::Request,
            [&](const std::optional<selecta::governor::Permit> &permit) { ++(permit ? granted : refused); });
    }
    ASSERT_TRUE(settle([&] { return granted + refused == asks; }));
    EXPECT_EQ(granted, selecta::governor::limits::kPeerLookupsInFlight);
    EXPECT_EQ(refused, 5);
}

TEST_F(NetworkServiceTest, SearchingOfflineSendsNothing)
{
    auto decision = governor.request(selecta::governor::Operation::Search, {.query = u"okuma drift"_s});
    ASSERT_TRUE(decision.permit.has_value());
    EXPECT_EQ(service->search(decision.cleanedQuery, std::move(*decision.permit)), 0U);
}

// --- NET-3: the port on the router, and peers that reach it ---------------------------------------------------------

TEST_F(NetworkServiceTest, PortMappingIsUnavailableUnlessEnabled)
{
    service->start(config(u"127.0.0.1"_s, 1));
    EXPECT_EQ(service->status().portMapping.state, selecta::core::PortMappingStatus::State::Unavailable);
    EXPECT_EQ(service->status().peersReachedUs, 0);
}

TEST_F(NetworkServiceTest, TheListeningPortIsMappedAndPeersThatReachItAreCounted)
{
    using MappingState = selecta::core::PortMappingStatus::State;
    selecta::core::testing::FakeNatPmp router;
    selecta::core::PortMapper::Options options;
    options.gateway = [] { return std::optional<QHostAddress>(QHostAddress(QHostAddress::LocalHost)); };
    options.natPmpPort = router.port();
    options.ssdp.reset();
    options.bindAddress = QHostAddress(QHostAddress::LocalHost);
    service->enablePortMapping(options);

    quint16 listenPort = 0;
    {
        QTcpServer probe; // a free port for the listener
        ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
        listenPort = probe.serverPort();
    }
    NetworkConfig withPort = config(u"127.0.0.1"_s, 1); // nothing listens on port 1: the session never starts
    withPort.listenPort = listenPort;
    service->start(withPort);
    EXPECT_EQ(service->status().portMapping.state, MappingState::Idle); // not listening yet
    service->setPassword(u"pw"_s);                                      // the attempt opens the listener
    ASSERT_TRUE(settle([&] { return service->status().portMapping.state == MappingState::Mapped; }));
    EXPECT_EQ(service->status().portMapping.internalPort, listenPort);
    EXPECT_EQ(router.mappings.value(listenPort), listenPort);

    const auto connectAs = [&](const QString &username) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, listenPort);
        EXPECT_TRUE(socket->waitForConnected(3000));
        socket->write(
            selecta::slsk::frame(selecta::slsk::peerinit::PeerInit{username, selecta::slsk::ConnectionType::Peer, 0}));
        return socket;
    };
    const auto alice = connectAs(u"alice"_s);
    ASSERT_TRUE(settle([&] { return service->status().peersReachedUs == 1; }));
    const auto aliceAgain = connectAs(u"alice"_s);
    const auto bob = connectAs(u"bob"_s);
    ASSERT_TRUE(settle([&] { return service->status().peersReachedUs == 2; }));
    QTest::qWait(100);
    EXPECT_EQ(service->status().peersReachedUs, 2); // alice counts once

    // Turned off in Settings: the mapping goes, the count stays.
    withPort.portMapping = false;
    service->setConfig(withPort);
    EXPECT_EQ(service->status().portMapping.state, MappingState::Off);
    ASSERT_TRUE(settle([&] { return router.mappings.isEmpty(); }));
    EXPECT_EQ(service->status().peersReachedUs, 2);
    service->disconnectNow();
}

TEST_F(NetworkServiceTest, EachPeerLookupIsLoggedOnceWithItsPurpose)
{
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log,
                     &selecta::core::ActivityLog::recordDecision);
    // As BrowseService does: the governor allows the browse, then slsk asks for the lookup to reach the peer.
    ASSERT_TRUE(governor.request(selecta::governor::Operation::Browse, {.peer = u"vinyl_ghost"_s}).allowed());
    bool called = false;
    service->permits().requestPeerLookup(u"vinyl_ghost"_s, selecta::slsk::ConnectPurpose::Request,
                                         [&](const std::optional<selecta::governor::Permit> &) { called = true; });
    ASSERT_TRUE(settle([&called] { return called; }));

    const auto entries = selecta::core::ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 2); // the browse, then one lookup line (not the governor's as well)
    EXPECT_EQ(entries[1].operation, u"browse"_s);
    EXPECT_EQ(entries[0].operation, u"peer-lookup"_s);
    EXPECT_EQ(entries[0].subject, u"vinyl_ghost"_s);
    EXPECT_EQ(entries[0].outcome, u"allowed"_s);
    EXPECT_EQ(entries[0].explanation, u"You looked up vinyl_ghost's address to browse their shares."_s);
}

TEST_F(NetworkServiceTest, ARefusedPeerLookupIsLoggedWithItsReason)
{
    int answered = 0;
    const int asks = selecta::governor::limits::kPeerLookupsInFlight + 1;
    for (int i = 0; i < asks; ++i) {
        service->permits().requestPeerLookup(u"searcher%1"_s.arg(i), selecta::slsk::ConnectPurpose::SearchResponse,
                                             [&](const std::optional<selecta::governor::Permit> &) { ++answered; });
    }
    ASSERT_TRUE(settle([&] { return answered == asks; }));
    const auto newest = selecta::core::ActivityLog::recent(db, 1);
    ASSERT_EQ(newest.size(), 1);
    EXPECT_EQ(newest[0].outcome, u"rate-limited"_s);
    EXPECT_TRUE(newest[0].explanation.startsWith(u"You wanted to look up searcher%1's address to send them your "
                                                 u"search results"_s.arg(asks - 1)))
        << newest[0].explanation.toStdString();
    EXPECT_TRUE(newest[0].explanation.contains(u"Selecta held it back: "_s));
}

// --- P-022: peer sockets leave descriptors for the rest of the app ---------------------------------------------------

namespace {

// Peers that only ever ask, through the server, to be dialled back.
class DialBackOnly : public selecta::slsk::testing::FakeServer::VirtualUser
{
public:
    void connectToPeerRequested(const selecta::slsk::fromserver::ConnectToPeerRequest & /*request*/) override {}
    void searchRequested(const selecta::slsk::fromserver::SearchRequest & /*request*/) override {}
    void cantConnectToPeer(quint32 /*token*/) override {}
};

} // namespace

TEST(NetworkServiceDescriptors, ADialBackStormLeavesDescriptorsForNewThreads)
{
#if !defined(Q_OS_UNIX)
    GTEST_SKIP() << "sockets don't count against an open-file limit on this system";
#else
    // "Find Them" on the live network brought ~1,100 relayed ConnectToPeer requests in two seconds (P-022). Each
    // dial-back took a descriptor until none was left, then a thread pool started a thread: Qt couldn't make the
    // thread's pipe and aborted the app.
    selecta::slsk::testing::FakeServer server;
    ASSERT_TRUE(server.start());
    // The peers never answer, so every dial-back keeps its descriptor, as it does while a real one connects.
    QTcpServer silentPeers;
    silentPeers.setListenBacklogSize(1024);
    ASSERT_TRUE(silentPeers.listen(QHostAddress::LocalHost, 0));
    silentPeers.pauseAccepting();
    DialBackOnly peer;
    constexpr int kStorm = 300;
    for (int i = 0; i < kStorm; ++i) {
        server.addVirtualUser(u"storm%1"_s.arg(i), QHostAddress::LocalHost, silentPeers.serverPort(), &peer);
    }

    // A low open-file limit, as macOS gives an app. The headroom stays under the listener's backlog (128 or more), so
    // the kernel completes every dial-back the limit allows and none is refused.
    const int openAtStart = selecta::testing::openDescriptorCount();
    ASSERT_GT(openAtStart, 0);
    const qint64 openFiles = openAtStart + 64;
    const selecta::testing::ScopedOpenFileLimit limit(openFiles);
    ASSERT_TRUE(limit.ok());

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    ASSERT_TRUE(db.open(u":memory:"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    NetworkService network(governor, log, tokens, [] { return qint64{1'800'000'000'000}; });
    NetworkConfig config;
    config.host = server.address().toString();
    config.port = server.port();
    config.username = u"selecta_test"_s;
    config.listenPort = 0;
    network.start(config);
    network.setPassword(u"pw"_s);
    ASSERT_TRUE(QTest::qWaitFor([&] { return network.isOnline(); }, 5000));

    const int openBeforeStorm = selecta::testing::openDescriptorCount();
    for (int i = 0; i < kStorm; ++i) {
        ASSERT_TRUE(server.relayConnectToPeer(u"storm%1"_s.arg(i), u"selecta_test"_s,
                                              selecta::slsk::ConnectionType::Peer, static_cast<quint32>(1000 + i)));
    }
    // Every request gets its answer: a dial-back holding its socket, or CantConnectToPeer.
    const auto answered = [&] {
        return server.receivedOf<selecta::slsk::toserver::CantConnectToPeer>(u"selecta_test"_s).size() +
               network.peers().socketCount();
    };
    ASSERT_TRUE(QTest::qWaitFor([&] { return answered() >= kStorm; }, 10'000)) << answered();

    // The storm took no more descriptors than the cap the limit allows, and that cap left the reserve alone.
    const int socketCap = selecta::governor::limits::socketCapFor(openFiles);
    EXPECT_GT(network.peers().socketCount(), 0);
    EXPECT_LE(network.peers().socketCount(), socketCap);
    EXPECT_LE(selecta::testing::openDescriptorCount() - openBeforeStorm, socketCap);

    // What aborted the app: a pool starting a thread, which needs a pipe of its own.
    QThreadPool pool;
    std::atomic<bool> ran{false};
    pool.start([&ran] { ran = true; });
    ASSERT_TRUE(pool.waitForDone(5000));
    EXPECT_TRUE(ran);
    network.disconnectNow();
#endif
}
