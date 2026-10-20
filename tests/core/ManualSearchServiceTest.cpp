// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ManualSearchService.h"

#include "base/Clock.h"
#include "governor/Governor.h"

#include <QPromise>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::ManualSearch;
using selecta::core::ManualSearchService;
using selecta::core::Ownership;
using selecta::core::SearchResult;
using selecta::core::SearchService;
using ResolutionResult = selecta::meta::Result<selecta::meta::Resolution>;
using State = ManualSearch::State;

namespace {

struct FakeNetwork final : selecta::core::SearchNetwork
{
    QStringList sent;
    QList<quint32> stopped;
    quint32 nextToken = 100;

    bool isOnline() const override { return true; }
    quint32 search(const QString &query,
                   selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        Q_UNUSED(permit)
        sent.append(query);
        return nextToken++;
    }
    void stopSearch(quint32 token) override { stopped.append(token); }
};

// A resolver whose answers the test hands out.
struct FakeResolver
{
    std::vector<std::unique_ptr<QPromise<ResolutionResult>>> pending;
    QList<selecta::meta::ResolveQuery> queries;

    ManualSearchService::ResolveFunction function()
    {
        return [this](const selecta::meta::ResolveQuery &query) {
            queries.append(query);
            auto promise = std::make_unique<QPromise<ResolutionResult>>();
            promise->start();
            QFuture<ResolutionResult> future = promise->future();
            pending.push_back(std::move(promise));
            return future;
        };
    }

    void answer(std::size_t index, ResolutionResult result)
    {
        pending.at(index)->addResult(std::move(result));
        pending.at(index)->finish();
    }
};

selecta::meta::Release release(const QString &artist, const QString &title, qint64 durationMs)
{
    selecta::meta::Release r;
    r.artists = {artist};
    r.title = title;
    r.album = u"Nightfall"_s;
    r.durationMs = durationMs;
    return r;
}

selecta::meta::Resolution resolution(const QList<selecta::meta::Release> &releases,
                                     selecta::meta::Confidence confidence)
{
    selecta::meta::Resolution r;
    for (const selecta::meta::Release &candidate : releases) {
        r.candidates.append(selecta::meta::Candidate{.release = candidate,
                                                     .score = 0.9,
                                                     .confidence = confidence,
                                                     .version = selecta::match::VersionCompatibility::Same,
                                                     .reasons = {}});
    }
    r.confidence = confidence;
    return r;
}

selecta::slsk::peer::FileSearchResponse answer(quint32 token, const QList<std::pair<QString, quint32>> &files)
{
    selecta::slsk::peer::FileSearchResponse response;
    response.token = token;
    for (const auto &[path, seconds] : files) {
        selecta::slsk::FileEntry file;
        file.name = selecta::slsk::RemotePath::fromText(path);
        file.size = (quint64{seconds} * 40'000) + 50'000; // 320 kbps
        file.attributes.bitrate = 320;
        file.attributes.duration = seconds;
        response.results.append(file);
    }
    response.slotFree = true;
    return response;
}

class ManualSearchServiceTest : public ::testing::Test
{
protected:
    ManualSearchServiceTest()
        : search(governor, network, clock, {.idle = 40ms, .total = 2000ms})
        , service(search)
    {
        service.setResolver(resolver.function());
        service.setLookupWait(200ms);
    }

    template <typename Condition>
    bool settle(Condition done)
    {
        return QTest::qWaitFor(
            [&] {
                service.waitForIdle();
                return done();
            },
            3000);
    }

    ManualSearch state(qint64 id) const { return service.search(id).value_or(ManualSearch{}); }

    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    FakeNetwork network;
    SearchService search;
    FakeResolver resolver;
    ManualSearchService service;
};

} // namespace

TEST_F(ManualSearchServiceTest, FindsTheReleaseThenSearchesSpelledAsIt)
{
    const QSignalSpy changes(&service, &ManualSearchService::searchChanged);
    const qint64 id = service.open(u"Okuma Drfit - Neon Rain"_s); // a typo
    ASSERT_GT(id, 0);
    EXPECT_EQ(state(id).state, State::LookingUp);
    ASSERT_EQ(resolver.queries.size(), 1);
    EXPECT_EQ(resolver.queries.constFirst().artist, u"Okuma Drfit"_s);
    EXPECT_EQ(resolver.queries.constFirst().title, u"Neon Rain"_s);
    EXPECT_TRUE(network.sent.isEmpty()); // the release comes first (SRCH-17)

    resolver.answer(
        0, resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Confident));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(network.sent.constFirst(), u"okuma drift neon rain"_s);
    const ManualSearch searching = state(id);
    EXPECT_EQ(searching.chosen, 0);
    EXPECT_TRUE(searching.chosenAutomatically);
    EXPECT_TRUE(searching.respelled); // SRCH-19: the summary says so
    EXPECT_GT(changes.size(), 1);
}

TEST_F(ManualSearchServiceTest, ResultsStreamInAndAreRankedAgainstTheRelease)
{
    const QSignalSpy added(&service, &ManualSearchService::resultsAdded);
    const QSignalSpy reset(&service, &ManualSearchService::resultsReset);
    const qint64 id = service.open(u"Okuma Drift - Neon Rain"_s);
    resolver.answer(0,
                    resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Exact));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));

    search.received(100, u"vinyl_ghost"_s, answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372}}));
    ASSERT_TRUE(settle([&] { return service.results(id).size() == 1; }));
    EXPECT_EQ(state(id).state, State::Searching);
    search.received(
        100, u"bassbin"_s,
        answer(100, {{u"@@b\\Okuma Drift - Neon Rain (Extended Mix).mp3"_s, 455}, {u"@@b\\cover.jpg"_s, 0}}));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done && service.results(id).size() == 3; }));

    const QList<SearchResult> results = service.results(id);
    EXPECT_EQ(results.at(0).index, 0);
    EXPECT_EQ(results.at(2).index, 2);
    EXPECT_TRUE(results.at(0).fitsRelease());
    EXPECT_FALSE(results.at(1).fitsRelease()); // another version, another length
    EXPECT_FALSE(results.at(2).isAudio());
    EXPECT_EQ(state(id).peers, 2);
    EXPECT_EQ(state(id).files, 3);
    // The first batch replaces what was on screen (nothing yet), later ones add to it.
    ASSERT_EQ(reset.size(), 1);
    EXPECT_EQ(reset.at(0).at(1).value<QList<SearchResult>>().size(), 1);
    EXPECT_EQ(added.size(), 1);
}

TEST_F(ManualSearchServiceTest, FreeTextThatNamesTheTrackPicksItsRelease)
{
    const qint64 id = service.open(u"okuma drift neon rain"_s);
    EXPECT_TRUE(resolver.queries.constFirst().artist.isEmpty());
    resolver.answer(
        0, resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Ambiguous));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(state(id).chosen, 0);
    EXPECT_TRUE(state(id).chosenAutomatically);
}

TEST_F(ManualSearchServiceTest, TextThatDoesntNameATrackLeavesTheChoiceToTheUser)
{
    const qint64 id = service.open(u"neon"_s);
    resolver.answer(0, resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000),
                                   release(u"Kassia Moreau"_s, u"Neon Lights"_s, 301'000)},
                                  selecta::meta::Confidence::Ambiguous));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(network.sent.constFirst(), u"neon"_s); // as typed
    const ManualSearch searched = state(id);
    EXPECT_EQ(searched.chosen, -1); // "Any (name only)"
    EXPECT_EQ(searched.candidates.size(), 2);
    EXPECT_FALSE(searched.respelled);
}

TEST_F(ManualSearchServiceTest, ALookupThatFailsOrIsSlowSearchesAsTyped)
{
    const qint64 failed = service.open(u"Okuma Drift - Neon Rain"_s);
    resolver.answer(0, ResolutionResult(selecta::meta::MetaError{.code = selecta::meta::ErrorCode::Network,
                                                                 .message = u"offline"_s}));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(network.sent.constFirst(), u"Okuma Drift Neon Rain"_s); // as typed, cleaned by the governor
    EXPECT_EQ(state(failed).chosen, -1);

    // A lookup that doesn't answer in time: the search goes as typed, and the release still ranks the results.
    const qint64 slow = service.open(u"Halden Rue - Tidewater"_s);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 2; }));
    EXPECT_EQ(state(slow).state, State::Searching);
    search.received(101, u"vinyl_ghost"_s, answer(101, {{u"@@a\\Halden Rue - Tidewater.mp3"_s, 301}}));
    ASSERT_TRUE(settle([&] { return service.results(slow).size() == 1; }));
    EXPECT_FALSE(service.results(slow).constFirst().fit.has_value());
    resolver.answer(
        1, resolution({release(u"Halden Rue"_s, u"Tidewater"_s, 301'000)}, selecta::meta::Confidence::Confident));
    ASSERT_TRUE(settle([&] {
        const QList<SearchResult> results = service.results(slow);
        return !results.isEmpty() && results.constFirst().fitsRelease();
    }));
    EXPECT_EQ(state(slow).chosen, 0);
    EXPECT_EQ(network.sent.size(), 2); // no second search for it
}

TEST_F(ManualSearchServiceTest, WithoutAResolverItSearchesAtOnce)
{
    ManualSearchService bare(search);
    const qint64 id = bare.open(u"okuma drift -live"_s);
    ASSERT_TRUE(QTest::qWaitFor([this] { return network.sent.size() == 1; }, 3000));
    EXPECT_EQ(network.sent.constFirst(), u"okuma drift -live"_s);
    EXPECT_EQ(bare.search(id)->chosen, -1);
}

TEST_F(ManualSearchServiceTest, ExclusionsGoAlongWithTheRelease)
{
    service.open(u"Okuma Drift - Neon Rain -live"_s);
    EXPECT_EQ(resolver.queries.constFirst().title, u"Neon Rain"_s);
    resolver.answer(
        0, resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Confident));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    EXPECT_EQ(network.sent.constFirst(), u"okuma drift neon rain -live"_s);
}

TEST_F(ManualSearchServiceTest, OneOpenSearchPerText)
{
    const qint64 first = service.open(u"Okuma Drift - Neon Rain"_s);
    EXPECT_EQ(service.open(u"  okuma drift -  neon rain "_s.simplified().replace(u"-  "_s, u"- "_s)), first);
    EXPECT_EQ(service.open(u"OKUMA DRIFT - NEON RAIN"_s), first);
    EXPECT_EQ(service.searches().size(), 1);
    EXPECT_EQ(service.open(u"   "_s), 0);
    EXPECT_EQ(service.open(u"-live"_s), 0); // only exclusions: nothing to search for
}

TEST_F(ManualSearchServiceTest, TheOldestSearchClosesBeyondTheLimit)
{
    const QSignalSpy closed(&service, &ManualSearchService::searchClosed);
    const qint64 first = service.open(u"search 0"_s);
    for (int i = 1; i <= ManualSearchService::kMaxOpen; ++i) {
        service.open(u"search %1"_s.arg(i));
    }
    EXPECT_EQ(service.searches().size(), ManualSearchService::kMaxOpen);
    EXPECT_FALSE(service.search(first).has_value());
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(closed.at(0).at(0).toLongLong(), first);
}

TEST_F(ManualSearchServiceTest, ChoosingAnotherReleaseRanksAgainWithoutSearching)
{
    const qint64 id = service.open(u"neon"_s);
    resolver.answer(0, resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000),
                                   release(u"Kassia Moreau"_s, u"Neon Lights"_s, 301'000)},
                                  selecta::meta::Confidence::Ambiguous));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(
        100, u"vinyl_ghost"_s,
        answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372}, {u"@@a\\Kassia Moreau - Neon Lights.mp3"_s, 301}}));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done && service.results(id).size() == 2; }));
    EXPECT_FALSE(service.results(id).at(0).fit.has_value());

    const QSignalSpy reset(&service, &ManualSearchService::resultsReset);
    service.chooseRelease(id, 1);
    ASSERT_TRUE(settle([&] { return !reset.isEmpty(); }));
    QList<SearchResult> results = service.results(id);
    EXPECT_FALSE(results.at(0).fitsRelease());
    EXPECT_TRUE(results.at(1).fitsRelease());
    EXPECT_EQ(state(id).chosen, 1);
    EXPECT_FALSE(state(id).chosenAutomatically);

    service.chooseRelease(id, -1);
    ASSERT_TRUE(settle([&] { return reset.size() == 2; }));
    results = service.results(id);
    EXPECT_FALSE(results.at(1).fit.has_value());
    EXPECT_EQ(network.sent.size(), 1);
}

TEST_F(ManualSearchServiceTest, SearchingAgainKeepsTheResultsUntilNewOnesCome)
{
    const qint64 id = service.open(u"okuma drift"_s);
    resolver.answer(0, ResolutionResult(resolution({}, selecta::meta::Confidence::None)));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372}}));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done; }));
    ASSERT_EQ(service.results(id).size(), 1);

    // Too soon: the governor refuses a repeat, and the results stay.
    service.refresh(id);
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done && !state(id).message.isEmpty(); }));
    EXPECT_EQ(service.results(id).size(), 1);
    EXPECT_EQ(network.sent.size(), 1);

    // Later it goes out again; the new answers replace the old ones.
    clock.advance(11min);
    const QSignalSpy reset(&service, &ManualSearchService::resultsReset);
    service.refresh(id);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 2; }));
    EXPECT_EQ(service.results(id).size(), 1); // still the old answer while waiting
    search.received(
        101, u"bassbin"_s,
        answer(101, {{u"@@b\\Okuma Drift - Neon Rain.flac"_s, 372}, {u"@@b\\Okuma Drift - Tidewater.flac"_s, 301}}));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done && service.results(id).size() == 2; }));
    EXPECT_EQ(service.results(id).constFirst().hit.peer, u"bassbin"_s);
    EXPECT_FALSE(reset.isEmpty());
}

TEST_F(ManualSearchServiceTest, SearchWiderSendsFewerWords)
{
    const qint64 id = service.open(u"Okuma Drift - Neon Rain"_s);
    resolver.answer(0,
                    resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Exact));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done; }));
    ASSERT_TRUE(state(id).canWiden);
    service.widen(id);
    ASSERT_TRUE(settle([this] { return network.sent.size() == 2; }));
    EXPECT_NE(network.sent.at(1), network.sent.at(0));
    EXPECT_LT(network.sent.at(1).split(u' ').size(), network.sent.at(0).split(u' ').size() + 1);
}

TEST_F(ManualSearchServiceTest, ClosingStopsTheSearch)
{
    const qint64 id = service.open(u"okuma drift"_s);
    resolver.answer(0, ResolutionResult(resolution({}, selecta::meta::Confidence::None)));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    const QSignalSpy closed(&service, &ManualSearchService::searchClosed);
    service.close(id);
    EXPECT_TRUE(network.stopped.contains(100U));
    EXPECT_EQ(closed.size(), 1);
    EXPECT_FALSE(service.search(id).has_value());
    search.received(100, u"late"_s, answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372}}));
    QTest::qWait(60);
    service.waitForIdle();
    EXPECT_TRUE(service.results(id).isEmpty());
}

TEST_F(ManualSearchServiceTest, ANewSnapshotSaysWhatYouHave)
{
    const qint64 id = service.open(u"okuma drift"_s);
    resolver.answer(0, ResolutionResult(resolution({}, selecta::meta::Confidence::None)));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s, answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372}}));
    ASSERT_TRUE(settle([&] { return service.results(id).size() == 1; }));
    EXPECT_EQ(service.results(id).constFirst().ownership, Ownership::None);

    selecta::match::MatchIndexInput input;
    selecta::match::LibraryTrack owned;
    owned.id = 11;
    owned.artist = u"Okuma Drift"_s;
    owned.title = u"Neon Rain"_s;
    owned.durationMs = 372'000;
    owned.quality.codec = selecta::match::Codec::Mp3;
    owned.quality.bitrateKbps = 320;
    input.tracks.append(owned);
    service.setLibraryIndex(selecta::match::MatchIndex::build(input));
    ASSERT_TRUE(settle([&] { return service.results(id).constFirst().ownership == Ownership::InCollection; }));
}

TEST_F(ManualSearchServiceTest, DownloadsAreTaggedFromTheReleaseTheyFit)
{
    QList<selecta::core::DownloadSpec> queued;
    service.setDownloads([&queued](const selecta::core::DownloadSpec &spec) {
        queued.append(spec);
        return true;
    });
    int annotated = 0;
    service.setAnnotate([&annotated](QList<SearchResult> &results) { annotated += static_cast<int>(results.size()); });
    const qint64 id = service.open(u"Okuma Drift - Neon Rain"_s);
    resolver.answer(0,
                    resolution({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)}, selecta::meta::Confidence::Exact));
    ASSERT_TRUE(settle([this] { return network.sent.size() == 1; }));
    search.received(100, u"vinyl_ghost"_s,
                    answer(100, {{u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 372},
                                 {u"@@a\\Halden Rue - Tidewater.mp3"_s, 301},
                                 {u"@@a\\cover.jpg"_s, 0}}));
    ASSERT_TRUE(settle([&] { return state(id).state == State::Done && service.results(id).size() == 3; }));
    EXPECT_GE(annotated, 3);

    EXPECT_EQ(service.download(id, {0, 2, 99}), 1); // the cover and a bad index are skipped
    ASSERT_EQ(queued.size(), 1);
    EXPECT_EQ(queued.at(0).peer, u"vinyl_ghost"_s);
    ASSERT_TRUE(queued.at(0).release.has_value());
    EXPECT_EQ(queued.at(0).release->title, u"Neon Rain"_s);
    ASSERT_TRUE(queued.at(0).file.has_value());
    EXPECT_TRUE(queued.at(0).file->fitsRelease);

    // Another track of the folder is looked up on its own before it goes.
    EXPECT_EQ(service.download(id, {1}), 1);
    ASSERT_EQ(resolver.queries.size(), 2);
    EXPECT_EQ(resolver.queries.at(1).artist, u"Halden Rue"_s);
    EXPECT_EQ(resolver.queries.at(1).title, u"Tidewater"_s);
    EXPECT_EQ(resolver.queries.at(1).durationMs, 301'000);
    resolver.answer(
        1, resolution({release(u"Halden Rue"_s, u"Tidewater"_s, 301'000)}, selecta::meta::Confidence::Confident));
    ASSERT_TRUE(settle([&] { return queued.size() == 2; }));
    ASSERT_TRUE(queued.at(1).release.has_value());
    EXPECT_EQ(queued.at(1).release->title, u"Tidewater"_s);
}
