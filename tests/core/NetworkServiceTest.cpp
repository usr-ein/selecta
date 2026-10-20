// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NetworkService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>
#include <QTest>

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
