// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchService.h"

#include "base/Clock.h"
#include "core/SearchCache.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QTest>

#include <gtest/gtest.h>
#include <optional>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::SearchOutcome;
using selecta::core::SearchService;
using selecta::governor::SearchSource;
namespace limits = selecta::governor::limits;

namespace {

struct FakeNetwork final : selecta::core::SearchNetwork
{
    bool online = true;
    QStringList sent;
    QList<quint32> stopped;
    quint32 nextToken = 100;

    bool isOnline() const override { return online; }
    // The permit is a move-only sink, taken by value like the interface.
    quint32 search(const QString &query,
                   selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        EXPECT_EQ(permit.operation(), selecta::governor::Operation::Search);
        sent.append(query);
        return nextToken++;
    }
    void stopSearch(quint32 token) override { stopped.append(token); }
};

selecta::slsk::peer::FileSearchResponse response(quint32 token, const QString &path, quint64 size = 15'000'000)
{
    selecta::slsk::peer::FileSearchResponse r;
    r.token = token;
    selecta::slsk::FileEntry file;
    file.name = selecta::slsk::RemotePath::fromText(path);
    file.size = size;
    file.attributes.bitrate = 320;
    file.attributes.duration = 372;
    r.results.append(file);
    r.slotFree = true;
    return r;
}

class SearchServiceTest : public ::testing::Test
{
protected:
    SearchServiceTest()
        : search(governor, network, clock, {.idle = 50ms, .total = 1000ms})
    {}

    // Queues a search and returns where its outcome will land.
    std::shared_ptr<std::optional<SearchOutcome>> queue(const QString &query,
                                                        SearchSource source = SearchSource::Manual)
    {
        auto outcome = std::make_shared<std::optional<SearchOutcome>>();
        search.enqueue(query, source, [outcome](const SearchOutcome &result) { *outcome = result; });
        return outcome;
    }

    template <typename Condition>
    static bool settle(Condition done)
    {
        return QTest::qWaitFor(done, 3000);
    }

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    FakeNetwork network;
    SearchService search;
};

} // namespace

TEST_F(SearchServiceTest, CollectsResultsUntilTheyStopComing)
{
    auto outcome = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(network.sent.constFirst(), u"okuma drift neon rain"_s);
    EXPECT_EQ(search.collecting(), 1);

    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    search.received(100, u"bassbin"_s, response(100, u"@@b\\Okuma Drift - Neon Rain.flac"_s));
    search.received(999, u"stranger"_s, response(999, u"@@c\\other.mp3"_s)); // another search's token
    ASSERT_TRUE(settle([&outcome] { return outcome->has_value(); }));
    EXPECT_EQ((*outcome)->hits.size(), 2);
    EXPECT_EQ((*outcome)->peers, 2);
    EXPECT_EQ((*outcome)->hits.constFirst().extension, u"mp3"_s);
    EXPECT_TRUE((*outcome)->error.isEmpty());
    EXPECT_TRUE(network.stopped.contains(100U));
}

TEST_F(SearchServiceTest, ARepeatComesFromTheCacheForFree)
{
    auto first = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    ASSERT_TRUE(settle([&first] { return first->has_value(); }));

    auto again = queue(u"Neon Rain Okuma Drift"_s); // same words, another order
    ASSERT_TRUE(settle([&again] { return again->has_value(); }));
    EXPECT_TRUE((*again)->fromCache);
    EXPECT_EQ((*again)->hits.size(), 1);
    EXPECT_EQ(network.sent.size(), 1);
}

TEST_F(SearchServiceTest, GetTracksLeavesRoomForSearchesTypedByHand)
{
    std::vector<std::shared_ptr<std::optional<SearchOutcome>>> batch;
    batch.reserve(limits::kSearchBurst);
    for (int i = 0; i < limits::kSearchBurst; ++i) {
        batch.push_back(queue(u"artist%1 title%1"_s.arg(10 + i), SearchSource::GetTracks));
    }
    const int allowed = limits::kSearchBurst - limits::kGetTracksReservedSearches;
    ASSERT_TRUE(settle([this] { return network.sent.size() == allowed; }));
    QTest::qWait(30);
    EXPECT_EQ(network.sent.size(), allowed);
    EXPECT_EQ(search.waiting(), limits::kGetTracksReservedSearches);

    // A search typed by hand still goes out at once.
    const auto manual = queue(u"halden rue tidewater"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == allowed + 1; }));
    EXPECT_EQ(network.sent.constLast(), u"halden rue tidewater"_s);
}

TEST_F(SearchServiceTest, NothingGoesOutOfflineOrPaused)
{
    network.online = false;
    const auto outcome = queue(u"okuma drift neon rain"_s);
    QTest::qWait(30);
    EXPECT_TRUE(network.sent.isEmpty());
    network.online = true;
    governor.setPaused(true);
    search.networkChanged();
    QTest::qWait(30);
    EXPECT_TRUE(network.sent.isEmpty());
    governor.setPaused(false);
    search.networkChanged();
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
}

TEST_F(SearchServiceTest, GoingOfflineEndsCollectionsWithWhatTheyHave)
{
    auto outcome = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    network.online = false;
    search.networkChanged();
    ASSERT_TRUE(settle([&outcome] { return outcome->has_value(); }));
    EXPECT_EQ((*outcome)->hits.size(), 1);
}

TEST_F(SearchServiceTest, CancelledSearchesDontAnswer)
{
    bool answered = false;
    network.online = false;
    const quint64 id = search.enqueue(u"okuma drift neon rain"_s, SearchSource::Manual,
                                      [&answered](const SearchOutcome &) { answered = true; });
    search.cancel(id);
    network.online = true;
    search.networkChanged();
    QTest::qWait(30);
    EXPECT_FALSE(answered);
    EXPECT_TRUE(network.sent.isEmpty());
}

TEST_F(SearchServiceTest, QueriesTheGovernorRefusesSayWhy)
{
    auto tooShort = queue(u"ab"_s);
    ASSERT_TRUE(settle([&tooShort] { return tooShort->has_value(); }));
    EXPECT_FALSE((*tooShort)->error.isEmpty());
    governor.setExcludedPhrases({u"neon rain"_s});
    auto excluded = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([&excluded] { return excluded->has_value(); }));
    EXPECT_FALSE((*excluded)->error.isEmpty());
    EXPECT_TRUE(network.sent.isEmpty());
}

TEST_F(SearchServiceTest, ResultsArriveAsPeersAnswer)
{
    QList<int> batches;
    QList<int> peers;
    std::optional<SearchOutcome> done;
    search.enqueue(
        SearchService::Request{.query = u"okuma drift neon rain"_s, .source = SearchSource::Manual, .refresh = false},
        [&done](const SearchOutcome &outcome) { done = outcome; }, {},
        [&](const QList<selecta::core::SearchHit> &added, int answered) {
            EXPECT_FALSE(done.has_value()); // every batch comes before the end
            batches.append(static_cast<int>(added.size()));
            peers.append(answered);
        });
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    search.received(100, u"bassbin"_s, response(100, u"@@b\\Okuma Drift - Neon Rain.flac"_s));
    ASSERT_TRUE(settle([&done] { return done.has_value(); }));
    EXPECT_EQ(batches, (QList<int>{1, 1}));
    EXPECT_EQ(peers, (QList<int>{1, 2}));
    EXPECT_EQ(done->hits.size(), 2);
}

TEST_F(SearchServiceTest, RefreshAsksTheNetworkDespiteTheCache)
{
    auto first = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    ASSERT_TRUE(settle([&first] { return first->has_value(); }));

    clock.advance(limits::kSameQueryQuiet + 1s); // the governor keeps a repeat quiet for a while
    std::optional<SearchOutcome> again;
    search.enqueue(
        SearchService::Request{.query = u"okuma drift neon rain"_s, .source = SearchSource::Manual, .refresh = true},
        [&again](const SearchOutcome &outcome) { again = outcome; });
    ASSERT_TRUE(settle([this] { return network.sent.size() == 2; }));
    search.received(101, u"bassbin"_s, response(101, u"@@b\\Okuma Drift - Neon Rain.flac"_s));
    ASSERT_TRUE(settle([&again] { return again.has_value(); }));
    EXPECT_FALSE(again->fromCache);
    ASSERT_EQ(again->hits.size(), 1);
    EXPECT_EQ(again->hits.constFirst().peer, u"bassbin"_s);
}

TEST_F(SearchServiceTest, KeptSearchesOutliveTheService)
{
    selecta::storage::Database db;
    ASSERT_TRUE(db.open(u":memory:"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    qint64 now = 1'800'000'000'000;
    selecta::core::SearchCacheStore store(db, [&now] { return now; });
    search.setStore(&store);
    auto first = queue(u"okuma drift neon rain"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, response(100, u"@@a\\Okuma Drift - Neon Rain.mp3"_s));
    ASSERT_TRUE(settle([&first] { return first->has_value(); }));
    EXPECT_EQ(store.count(), 1);

    // A new service (the next launch), hours later: the answer comes from the database, labelled with its age.
    now += qint64{3} * 60 * 60 * 1000;
    const selecta::base::ManualClock laterClock;
    selecta::governor::Governor laterGovernor{laterClock};
    FakeNetwork laterNetwork;
    SearchService later(laterGovernor, laterNetwork, laterClock, {.idle = 50ms, .total = 1000ms});
    later.setStore(&store);
    std::optional<SearchOutcome> cached;
    later.enqueue(u"okuma drift neon rain"_s, SearchSource::Manual,
                  [&cached](const SearchOutcome &outcome) { cached = outcome; });
    ASSERT_TRUE(settle([&cached] { return cached.has_value(); }));
    EXPECT_TRUE(cached->fromCache);
    EXPECT_EQ(cached->searchedAtMs, 1'800'000'000'000);
    EXPECT_EQ(cached->hits.size(), 1);
    EXPECT_TRUE(laterNetwork.sent.isEmpty());
    later.setStore(nullptr);
    search.setStore(nullptr);
}
