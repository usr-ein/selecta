// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GetTracksService.h"

#include "base/Clock.h"
#include "core/ReleaseJson.h"
#include "core/SearchService.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QPromise>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::core::GetLine;
using selecta::core::GetTracksService;
using ReleaseState = GetLine::ReleaseState;
using ResolutionResult = selecta::meta::Result<selecta::meta::Resolution>;

namespace {

selecta::meta::Release release(const QString &artist, const QString &title, qint64 durationMs)
{
    selecta::meta::Release r;
    r.artists = {artist};
    r.title = title;
    r.album = u"Nightfall"_s;
    r.durationMs = durationMs;
    r.isrc = u"GBXXX2400001"_s;
    return r;
}

// A resolver whose answers the test hands out by hand.
struct FakeResolver
{
    std::vector<std::unique_ptr<QPromise<ResolutionResult>>> pending;
    QList<selecta::meta::ResolveQuery> queries;

    GetTracksService::ResolveFunction function()
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

selecta::meta::Resolution resolution(const selecta::meta::Release &best, selecta::meta::Confidence confidence)
{
    selecta::meta::Resolution r;
    r.candidates.append(selecta::meta::Candidate{.release = best,
                                                 .score = 0.95,
                                                 .confidence = confidence,
                                                 .version = selecta::match::VersionCompatibility::Same,
                                                 .reasons = {}});
    r.confidence = confidence;
    return r;
}

class GetTracksServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        service = std::make_unique<GetTracksService>(db, [] { return qint64{1'800'000'000'000}; });
    }

    // Lets queued continuations run.
    static void settle() { QTest::qWait(5); }

    selecta::storage::Database db;
    std::unique_ptr<GetTracksService> service;
    FakeResolver resolver;
};

} // namespace

TEST_F(GetTracksServiceTest, AddParsesDeduplicatesAndSkipsUnidentified)
{
    const auto result = service->add(u"1. Okuma Drift - Neon Rain\n"
                                     u"OKUMA DRIFT - neon rain\n"
                                     u"ID - ID\n"
                                     u"\n"
                                     u"Halden & Rue - Tidewater (Original Mix) 6:12\n"_s);
    EXPECT_EQ(result.added, 2);
    EXPECT_EQ(result.unidentified, 1);
    const auto lines = service->lines();
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines[0].artist, u"Okuma Drift"_s);
    EXPECT_EQ(lines[0].title, u"Neon Rain"_s);
    EXPECT_EQ(lines[1].title, u"Tidewater (Original Mix)"_s);
    EXPECT_EQ(lines[1].durationHintMs, 372'000);
    EXPECT_EQ(lines[0].releaseState, ReleaseState::Pending);

    const auto again = service->add(u"Okuma Drift - Neon Rain\nHalden & Rue - Tidewater (Original Mix)"_s);
    EXPECT_EQ(again.added, 0);
    EXPECT_EQ(again.duplicates, 2);
}

TEST_F(GetTracksServiceTest, TheListSurvivesARestart)
{
    service->add(u"A - One\nB - Two\nC - Three"_s);
    service.reset();
    GetTracksService restarted(db);
    const auto lines = restarted.lines();
    ASSERT_EQ(lines.size(), 3);
    EXPECT_EQ(lines[2].artist, u"C"_s);
    EXPECT_LT(lines[0].position, lines[1].position);
}

TEST_F(GetTracksServiceTest, AResolutionGivesTheLineItsRelease)
{
    service->add(u"Okuma Drift - Neon Rain"_s);
    service->setResolver(resolver.function());
    ASSERT_EQ(resolver.queries.size(), 1);
    EXPECT_EQ(resolver.queries[0].artist, u"Okuma Drift"_s);
    const QSignalSpy changed(service.get(), &GetTracksService::lineChanged);

    resolver.answer(
        0, resolution(release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000), selecta::meta::Confidence::Confident));
    settle();
    ASSERT_EQ(changed.size(), 1);
    const GetLine line = service->lines().constFirst();
    EXPECT_EQ(line.releaseState, ReleaseState::Resolved);
    ASSERT_TRUE(line.release.has_value());
    EXPECT_EQ(line.release->durationMs, 372'000);
    EXPECT_EQ(line.releaseConfidence, u"confident"_s);

    // Saved: a new service sees the release without resolving again.
    service.reset();
    GetTracksService restarted(db);
    ASSERT_TRUE(restarted.lines().constFirst().release.has_value());
    EXPECT_EQ(restarted.lines().constFirst().release->title, u"Neon Rain"_s);
}

TEST_F(GetTracksServiceTest, AmbiguousNothingAndFailures)
{
    service->add(u"A - One\nB - Two\nC - Three"_s);
    service->setResolver(resolver.function());
    ASSERT_EQ(resolver.pending.size(), 3U);
    resolver.answer(0, resolution(release(u"A"_s, u"One"_s, 1000), selecta::meta::Confidence::Ambiguous));
    resolver.answer(1, selecta::meta::Resolution{});
    resolver.answer(2, selecta::meta::makeError(selecta::meta::Service::Deezer, selecta::meta::ErrorCode::Network,
                                                u"no connection"_s));
    settle();
    const auto lines = service->lines();
    EXPECT_EQ(lines[0].releaseState, ReleaseState::Ambiguous);
    EXPECT_TRUE(lines[0].release.has_value());
    EXPECT_EQ(lines[1].releaseState, ReleaseState::None);
    EXPECT_FALSE(lines[1].release.has_value());
    EXPECT_EQ(lines[2].releaseState, ReleaseState::Failed);
    EXPECT_EQ(lines[2].message, u"no connection"_s);

    service->retry({lines[2].id});
    EXPECT_EQ(resolver.pending.size(), 4U);
    EXPECT_EQ(service->lines().at(2).releaseState, ReleaseState::Pending);
}

TEST_F(GetTracksServiceTest, AtMostThreeResolveAtOnce)
{
    service->add(u"A - 1a\nB - 2b\nC - 3c\nD - 4d\nE - 5e"_s);
    service->setResolver(resolver.function());
    EXPECT_EQ(resolver.pending.size(), 3U);
    EXPECT_EQ(service->resolvingCount(), 3);
    resolver.answer(0, selecta::meta::Resolution{});
    settle();
    EXPECT_EQ(resolver.pending.size(), 4U);
    EXPECT_EQ(service->resolvingCount(), 3);
}

TEST_F(GetTracksServiceTest, RemovingALineCancelsItsResolution)
{
    service->add(u"A - One\nB - Two"_s);
    service->setResolver(resolver.function());
    ASSERT_EQ(resolver.pending.size(), 2U);
    const qint64 first = service->lines().constFirst().id;
    service->remove({first});
    EXPECT_TRUE(resolver.pending.at(0)->isCanceled());
    EXPECT_EQ(service->lines().size(), 1);
    EXPECT_EQ(service->resolvingCount(), 1);
}

TEST_F(GetTracksServiceTest, PlaylistTracksArriveResolved)
{
    service->setResolver(resolver.function());
    const auto result = service->addReleases(
        {release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000), release(u"Halden & Rue"_s, u"Tidewater"_s, 401'000)});
    EXPECT_EQ(result.added, 2);
    EXPECT_TRUE(resolver.queries.isEmpty());
    const auto lines = service->lines();
    EXPECT_EQ(lines[1].releaseState, ReleaseState::Resolved);
    EXPECT_EQ(lines[1].durationHintMs, 401'000);
}

TEST_F(GetTracksServiceTest, OnlyLinesWithAFileCanBeTicked)
{
    service->add(u"A - One"_s);
    const qint64 id = service->lines().constFirst().id;
    service->setTicked({id}, true);
    EXPECT_FALSE(service->lines().constFirst().ticked);
}

TEST_F(GetTracksServiceTest, ChoosingAReleaseReplacesTheResolution)
{
    service->add(u"A - One"_s);
    service->setResolver(resolver.function());
    const qint64 id = service->lines().constFirst().id;
    service->chooseRelease(id, release(u"A"_s, u"One (Extended Mix)"_s, 420'000));
    EXPECT_TRUE(resolver.pending.at(0)->isCanceled());
    EXPECT_EQ(service->lines().constFirst().release->title, u"One (Extended Mix)"_s);
    EXPECT_EQ(service->lines().constFirst().releaseConfidence, u"chosen"_s);
}

TEST_F(GetTracksServiceTest, ClearEmptiesTheList)
{
    service->add(u"A - One\nB - Two"_s);
    const QSignalSpy reset(service.get(), &GetTracksService::linesReset);
    service->clear();
    EXPECT_TRUE(service->lines().isEmpty());
    EXPECT_EQ(reset.size(), 1);
    EXPECT_EQ(selecta::core::GetTracksStore(db).count(), 0);
}

TEST(ReleaseJson, EveryFieldRoundTrips)
{
    selecta::meta::Release r = release(u"Okuma Drift"_s, u"Neon Rain (Extended Mix)"_s, 372'000);
    r.source = selecta::meta::Source::Deezer;
    r.artists = {u"Okuma Drift"_s, u"Ines Varga"_s};
    r.albumArtists = {u"Okuma Drift"_s};
    r.albumType = u"album"_s;
    r.releaseDate = u"2024-03"_s;
    r.releaseDatePrecision = selecta::meta::DatePrecision::Month;
    r.trackNumber = 3;
    r.trackTotal = 12;
    r.discNumber = 1;
    r.discTotal = 2;
    r.upc = u"0123456789012"_s;
    r.covers = {
        selecta::meta::CoverRef{.url = QUrl(u"https://example.invalid/640.jpg"_s), .width = 640, .height = 640}};
    r.spotifyId = u"6rqhFgbbKwnb9MLmUQDhG6"_s;
    r.spotifyUrl = QUrl(u"https://open.spotify.com/track/6rqhFgbbKwnb9MLmUQDhG6"_s);
    r.deezerId = u"12345"_s;
    r.musicBrainzRecordingId = u"0f0e0d0c-0000-0000-0000-000000000000"_s;
    r.label = u"Night Shift Records"_s;
    r.catalogNumber = u"NSR042"_s;
    r.genres = {u"house"_s, u"deep house"_s};
    r.labelSource = selecta::meta::Source::MusicBrainz;
    r.genreSource = selecta::meta::Source::MusicBrainz;
    r.copyrightP = u"℗ 2024 Night Shift Records"_s;

    const auto back = selecta::core::releaseFromText(selecta::core::releaseToText(r));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, r);
    EXPECT_FALSE(selecta::core::releaseFromText(QByteArray("not json")).has_value());
    EXPECT_FALSE(selecta::core::releaseFromText(QByteArray()).has_value());
}

namespace {

struct FindNetwork final : selecta::core::SearchNetwork
{
    QStringList sent;
    bool isOnline() const override { return true; }
    // The permit is a move-only sink, taken by value like the interface.
    quint32 search(const QString &query,
                   [[maybe_unused]] selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        sent.append(query);
        return static_cast<quint32>(sent.size());
    }
    void stopSearch([[maybe_unused]] quint32 token) override {}
};

selecta::slsk::peer::FileSearchResponse fileResponse(quint32 token, const QString &path, quint32 bitrate,
                                                     quint32 durationSec)
{
    selecta::slsk::peer::FileSearchResponse r;
    r.token = token;
    selecta::slsk::FileEntry file;
    file.name = selecta::slsk::RemotePath::fromText(path);
    file.size = quint64{bitrate} * 1000 / 8 * durationSec;
    file.attributes.bitrate = bitrate;
    file.attributes.duration = durationSec;
    r.results.append(file);
    r.slotFree = true;
    return r;
}

} // namespace

TEST_F(GetTracksServiceTest, FindingTicksConfidentMatchesAndFlagsTheRest)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    FindNetwork network;
    selecta::core::SearchService search(
        governor, network, clock, {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)});
    service->setSearch(&search, [] { return QStringList(); });
    service->addReleases({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000),
                          release(u"Halden Rue"_s, u"Tidewater"_s, 401'000),
                          release(u"Kassia Moreau"_s, u"Night Transit"_s, 333'000)});
    service->find();
    ASSERT_TRUE(QTest::qWaitFor([&network] { return network.sent.size() == 3; }, 3000));
    const auto searching = service->lines();
    for (const auto &line : searching) {
        EXPECT_EQ(line.findState, GetLine::FindState::Searching);
    }

    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372));
    search.received(2, u"bassbin"_s, fileResponse(2, u"@@b\\Halden Rue - Tidewater.mp3"_s, 320, 330)); // wrong length
    // Nothing for the third.
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    const auto lines = service->lines();
    EXPECT_EQ(lines[0].findState, GetLine::FindState::Ready);
    EXPECT_TRUE(lines[0].ticked);
    ASSERT_TRUE(lines[0].best.has_value());
    EXPECT_EQ(lines[0].best->peer, u"vinyl_ghost"_s);
    EXPECT_NE(lines[1].findState, GetLine::FindState::Ready);
    EXPECT_FALSE(lines[1].ticked);
    EXPECT_EQ(lines[2].findState, GetLine::FindState::NotFound);

    // The chosen file is saved with the line.
    service.reset();
    GetTracksService restarted(db);
    ASSERT_TRUE(restarted.lines().constFirst().best.has_value());
    EXPECT_TRUE(restarted.lines().constFirst().ticked);
}

TEST_F(GetTracksServiceTest, StoppingPutsQueuedLinesBack)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    FindNetwork network;
    selecta::core::SearchService search(governor, network, clock);
    service->setSearch(&search, [] { return QStringList(); });
    governor.setPaused(true); // nothing can go out
    service->addReleases({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)});
    service->find();
    EXPECT_EQ(service->lines().constFirst().findState, GetLine::FindState::Queued);
    service->stopFinding();
    EXPECT_EQ(service->lines().constFirst().findState, GetLine::FindState::Idle);
    EXPECT_FALSE(service->isFinding());
}

TEST_F(GetTracksServiceTest, AnotherPresetRanksTheFoundLinesAgain)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    FindNetwork network;
    selecta::core::SearchService search(
        governor, network, clock, {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)});
    service->setSearch(&search, [] { return QStringList(); });
    service->addReleases({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)});
    service->find();
    ASSERT_TRUE(QTest::qWaitFor([&network] { return network.sent.size() == 1; }, 3000));
    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372));
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    ASSERT_EQ(service->lines().constFirst().findState, GetLine::FindState::Ready);

    // Lossless only (SRCH-8): the line is ranked again from the results the search just had, without searching the
    // network again, and the MP3 no longer qualifies.
    const selecta::core::SearchFilter lossless =
        selecta::core::builtInPreset(selecta::core::kLosslessPreset).value().filter;
    service->setFilter(lossless);
    ASSERT_TRUE(QTest::qWaitFor(
        [this] { return service->lines().constFirst().findState == GetLine::FindState::NotFound; }, 3000));
    const GetLine line = service->lines().constFirst();
    EXPECT_FALSE(line.ticked);
    EXPECT_TRUE(line.message.contains(u"quality wanted"_s)) << line.message.toStdString();
    EXPECT_EQ(network.sent.size(), 1);

    // The same preset again changes nothing.
    service->setFilter(lossless);
    QTest::qWait(100);
    EXPECT_EQ(network.sent.size(), 1);
    EXPECT_EQ(service->lines().constFirst().findState, GetLine::FindState::NotFound);
}

TEST(FindingTimeLeft, FollowsTheGovernorsPace)
{
    using std::chrono::milliseconds;
    using namespace std::chrono_literals;
    const auto collect = std::chrono::duration_cast<milliseconds>(selecta::governor::limits::kResultsIdle);
    const auto refill = std::chrono::duration_cast<milliseconds>(selecta::governor::limits::kSearchRefill);
    const int reserve = selecta::governor::limits::kGetTracksReservedSearches;
    // Nothing left: nothing to wait for; only collecting: the answers' tail.
    EXPECT_EQ(selecta::core::findingTimeLeft(0, 0, 6, 0ms), 0ms);
    EXPECT_EQ(selecta::core::findingTimeLeft(0, 2, 6, 0ms), collect);
    // What the budget holds beyond the reserve starts at once.
    EXPECT_EQ(selecta::core::findingTimeLeft(3, 0, 3 + reserve, 0ms), collect);
    // The rest at one per refill, the first when the governor says.
    EXPECT_EQ(selecta::core::findingTimeLeft(5, 0, reserve, 10s), 10s + (4 * refill) + collect);
    EXPECT_EQ(selecta::core::findingTimeLeft(1, 0, 0, 0ms), refill + collect);
    // A wait longer than a refill is capped at a refill.
    EXPECT_EQ(selecta::core::findingTimeLeft(1, 0, 0, 10min), refill + collect);
}

TEST(SpreadChoice, ABatchSpreadsOverPeersWithTheSameFile)
{
    const auto choice = [](const QString &peer, double fit, bool freeSlot) {
        selecta::core::FileChoice file;
        file.peer = peer;
        file.fit = fit;
        file.fitsRelease = true;
        file.freeSlot = freeSlot;
        return file;
    };
    const selecta::core::FileChoice best = choice(u"lena"_s, 0.95, true);
    const QList<selecta::core::FileChoice> alternatives = {choice(u"vera"_s, 0.93, true),
                                                           choice(u"hugo"_s, 0.94, true)};
    // Nobody loaded: the best stays.
    EXPECT_EQ(selecta::core::spreadChoice(best, alternatives, {}), -1);
    // Its peer already has one of the batch: the least loaded peer with as good a file takes it.
    EXPECT_EQ(selecta::core::spreadChoice(best, alternatives, {{u"lena"_s, 1}}), 0);
    EXPECT_EQ(selecta::core::spreadChoice(best, alternatives, {{u"lena"_s, 2}, {u"vera"_s, 1}}), 1);
    // A clearly worse fit, a busy slot when the best has a free one, or another fit status never takes it.
    EXPECT_EQ(selecta::core::spreadChoice(best, {choice(u"vera"_s, 0.80, true)}, {{u"lena"_s, 3}}), -1);
    EXPECT_EQ(selecta::core::spreadChoice(best, {choice(u"vera"_s, 0.95, false)}, {{u"lena"_s, 3}}), -1);
    selecta::core::FileChoice unsure = choice(u"vera"_s, 0.95, true);
    unsure.fitsRelease = false;
    EXPECT_EQ(selecta::core::spreadChoice(best, {unsure}, {{u"lena"_s, 3}}), -1);
}

TEST(FileChoiceText, TheShortfallRoundTripsAndOldRowsReadWithout)
{
    selecta::core::FileChoice choice;
    choice.peer = u"low_bitrate"_s;
    choice.remotePath = "@@l\\Okuma Drift - Neon Rain.mp3";
    choice.size = 8'928'000;
    choice.extension = u"mp3"_s;
    choice.bitrate = 192;
    choice.fitsRelease = true;
    choice.shortfall = u"under 320 kbps"_s;
    const auto back = selecta::core::fileChoiceFromText(selecta::core::fileChoiceToText(choice));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, choice);

    choice.shortfall.clear();
    const QByteArray text = selecta::core::fileChoiceToText(choice);
    EXPECT_FALSE(text.contains("shortfall")); // rows of files that passed stay as they were
    EXPECT_EQ(selecta::core::fileChoiceFromText(text)->shortfall, QString());
}

TEST(SpreadChoice, AFileGotAnywayStaysAndNearMissesAreNeverSpreadTo)
{
    selecta::core::FileChoice best;
    best.peer = u"lena"_s;
    best.fit = 0.95;
    best.fitsRelease = true;
    best.freeSlot = true;
    best.shortfall = u"under 320 kbps"_s;
    selecta::core::FileChoice other = best;
    other.peer = u"vera"_s;
    EXPECT_EQ(selecta::core::spreadChoice(best, {other}, {{u"lena"_s, 3}}), -1);
    best.shortfall.clear(); // a file that passed: a near miss as good is still never taken instead
    EXPECT_EQ(selecta::core::spreadChoice(best, {other}, {{u"lena"_s, 3}}), -1);
}

TEST_F(GetTracksServiceTest, ALineNotFoundKeepsItsNearMissesForThisSession)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    FindNetwork network;
    selecta::core::SearchService search(
        governor, network, clock, {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)});
    service->setSearch(&search, [] { return QStringList(); });
    service->addReleases({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)});
    service->find();
    ASSERT_TRUE(QTest::qWaitFor([&network] { return network.sent.size() == 1; }, 3000));
    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 192, 372));
    search.received(1, u"bassbin"_s, fileResponse(1, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, 128, 372));
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    GetLine line = service->lines().constFirst();
    EXPECT_EQ(line.findState, GetLine::FindState::NotFound);
    EXPECT_FALSE(line.best.has_value());
    EXPECT_FALSE(line.ticked);
    EXPECT_TRUE(line.foundThisSession);
    ASSERT_EQ(line.nearMisses.size(), 2);
    EXPECT_EQ(line.nearMisses[0].peer, u"vinyl_ghost"_s);
    EXPECT_EQ(line.nearMisses[0].shortfall, u"under 320 kbps"_s);
    EXPECT_EQ(line.message, u"2 files of this track found, below the quality wanted."_s);

    // Without a transfer service, nothing can be got: Get anyway and Try another peer do nothing.
    service->getAnyway(line.id, line.nearMisses[0].peer, line.nearMisses[0].remotePath);
    service->switchPeer(line.id);
    EXPECT_EQ(service->lines().constFirst().nearMisses.size(), 2);
    EXPECT_FALSE(service->lines().constFirst().best.has_value());

    // Like the alternatives, they aren't saved: after a restart the line says what was found, and finding it again
    // (the search cache answers) brings them back.
    service.reset();
    GetTracksService restarted(db);
    line = restarted.lines().constFirst();
    EXPECT_TRUE(line.nearMisses.isEmpty());
    EXPECT_FALSE(line.foundThisSession);
    EXPECT_EQ(line.message, u"2 files of this track found, below the quality wanted."_s);
}

TEST_F(GetTracksServiceTest, FilesOfAnotherTrackAreNoNearMisses)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    FindNetwork network;
    selecta::core::SearchService search(
        governor, network, clock, {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)});
    service->setSearch(&search, [] { return QStringList(); });
    service->addReleases({release(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000)});
    service->find();
    ASSERT_TRUE(QTest::qWaitFor([&network] { return network.sent.size() == 1; }, 3000));
    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain (Live).mp3"_s, 192, 512));
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    const GetLine line = service->lines().constFirst();
    EXPECT_EQ(line.findState, GetLine::FindState::NotFound);
    EXPECT_TRUE(line.nearMisses.isEmpty());
    EXPECT_EQ(line.message, u"1 file found, but not of this track."_s);
}

// --- Upgrades (P-010) ------------------------------------------------------------------------------------------------

namespace {

selecta::core::FileChoice choice(const QString &extension, std::optional<int> bitrate, std::optional<bool> vbr = false,
                                 std::optional<int> bitDepth = std::nullopt)
{
    selecta::core::FileChoice file;
    file.extension = extension;
    file.bitrate = bitrate;
    file.vbr = vbr;
    file.bitDepth = bitDepth;
    return file;
}

GetTracksService::UpgradeRequest upgrade(const QString &path, const QString &artist, const QString &title,
                                         qint64 durationMs = 372'000)
{
    return GetTracksService::UpgradeRequest{
        .path = path, .artist = artist, .title = title, .durationMs = durationMs, .isrc = u"GBXXX2400001"_s};
}

// A search harness: the lines' searches go to `network`, answered by hand.
struct Finding
{
    explicit Finding(GetTracksService &service)
    {
        service.setSearch(&search, [] { return QStringList(); });
    }

    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
    FindNetwork network;
    selecta::core::SearchService search{
        governor, network, clock, {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)}};
};

} // namespace

TEST(QualityTierOf, WhatAPeerDeclares)
{
    using selecta::core::qualityTierOf;
    using selecta::core::QualityTier;
    EXPECT_EQ(qualityTierOf(choice(u"mp3"_s, 320)), QualityTier::Top);
    EXPECT_EQ(qualityTierOf(choice(u"mp3"_s, 245, true)), QualityTier::Top); // V0
    EXPECT_EQ(qualityTierOf(choice(u"mp3"_s, 192)), QualityTier::From192);
    EXPECT_EQ(qualityTierOf(choice(u"mp3"_s, std::nullopt)), QualityTier::Unknown);
    EXPECT_EQ(qualityTierOf(choice(u"flac"_s, std::nullopt, std::nullopt)), QualityTier::Lossless);
    EXPECT_EQ(qualityTierOf(choice(u"m4a"_s, std::nullopt, std::nullopt, 16)), QualityTier::Lossless); // ALAC
    EXPECT_EQ(qualityTierOf(choice(u"m4a"_s, 256)), QualityTier::From256);                             // AAC
}

TEST_F(GetTracksServiceTest, UpgradesAreOneLinePerFileSearchedAtOnce)
{
    Finding finding(*service);
    service->add(u"Okuma Drift - Neon Rain"_s); // an ordinary line for the same track doesn't stop an upgrade
    const QSignalSpy reset(service.get(), &GetTracksService::linesReset);
    const auto result = service->addUpgrades({
        upgrade(u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s),
        upgrade(u"/Music/Albums/Nightfall/01 Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s), // the same, twice
        upgrade(u"/Music/Tracks/untagged.mp3"_s, QString(), QString()),
        upgrade(QString(), u"Nobody"_s, u"Nowhere"_s), // no file: nothing to upgrade
    });
    EXPECT_EQ(result.lines.size(), 3);
    EXPECT_EQ(result.already, 0);
    EXPECT_FALSE(reset.isEmpty());
    const QList<GetLine> lines = service->lines();
    ASSERT_EQ(lines.size(), 4);
    EXPECT_TRUE(lines.at(0).upgradeOf.isEmpty());
    EXPECT_EQ(lines.at(1).upgradeOf, u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s);
    EXPECT_EQ(lines.at(1).raw, u"Okuma Drift - Neon Rain"_s);
    EXPECT_EQ(lines.at(1).durationHintMs, 372'000);
    EXPECT_EQ(lines.at(1).isrc, u"GBXXX2400001"_s);
    EXPECT_EQ(lines.at(3).raw, u"untagged"_s); // named after the file
    // Searched at once (no release lookup here), by name; the ordinary line waits for its lookup.
    ASSERT_TRUE(QTest::qWaitFor([&finding] { return !finding.network.sent.isEmpty(); }, 3000));
    EXPECT_TRUE(finding.network.sent.constFirst().contains(u"neon rain"_s, Qt::CaseInsensitive))
        << finding.network.sent.constFirst().toStdString();
    EXPECT_EQ(service->lines().at(0).findState, GetLine::FindState::Idle);

    // Asked again for the same files: the list already looks for them.
    const auto again =
        service->addUpgrades({upgrade(u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s)});
    EXPECT_TRUE(again.lines.isEmpty());
    EXPECT_EQ(again.already, 1);

    // The mark is saved with the line.
    service.reset();
    GetTracksService restarted(db);
    EXPECT_EQ(restarted.lines().at(1).upgradeOf, u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s);
}

TEST_F(GetTracksServiceTest, AnUpgradeIsNeverYouHaveIt)
{
    selecta::match::MatchIndexInput input;
    selecta::match::LibraryTrack owned;
    owned.id = 1;
    owned.artist = u"Okuma Drift"_s;
    owned.title = u"Neon Rain"_s;
    owned.durationMs = 372'000;
    owned.quality.codec = selecta::match::Codec::Mp3;
    owned.quality.bitrateKbps = 128;
    input.tracks.append(owned);
    service->setLibraryIndex(selecta::match::MatchIndex::build(input));
    service->add(u"Okuma Drift - Neon Rain"_s);
    service->addUpgrades({upgrade(u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s)});
    const QList<GetLine> lines = service->lines();
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines.at(0).findState, GetLine::FindState::Owned); // the ordinary line: you have it
    EXPECT_NE(lines.at(1).findState, GetLine::FindState::Owned);
    service->recheckOwnership();
    EXPECT_NE(service->lines().at(1).findState, GetLine::FindState::Owned);
}

TEST_F(GetTracksServiceTest, AnUpgradeTakesOnlyAHigherTier)
{
    Finding finding(*service);
    selecta::core::QualityTier floor = selecta::core::QualityTier::Top;
    QStringList asked;
    service->setTierLookup([&floor, &asked](const QString &path) -> std::optional<selecta::core::QualityTier> {
        asked.append(path);
        return floor;
    });
    const QString path = u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s;
    service->addUpgrades({upgrade(path, u"Okuma Drift"_s, u"Neon Rain"_s)});
    ASSERT_TRUE(QTest::qWaitFor([&finding] { return finding.network.sent.size() == 1; }, 3000));
    selecta::core::SearchService &search = finding.search;
    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372));
    search.received(1, u"flac_fiend"_s, fileResponse(1, u"@@b\\Okuma Drift - Neon Rain.flac"_s, 900, 372));
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    EXPECT_EQ(asked, QStringList{path});
    // Yours is a 320: only the FLAC is better.
    GetLine line = service->lines().constFirst();
    ASSERT_TRUE(line.best.has_value());
    EXPECT_EQ(line.best->peer, u"flac_fiend"_s);
    EXPECT_TRUE(line.alternatives.isEmpty());

    // Yours is from lossless already: nothing found is better, and the line says so.
    floor = selecta::core::QualityTier::Lossless;
    service->find({line.id});
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    line = service->lines().constFirst();
    EXPECT_EQ(line.findState, GetLine::FindState::NotFound);
    EXPECT_FALSE(line.best.has_value());
    EXPECT_TRUE(line.nearMisses.isEmpty());
    EXPECT_EQ(line.message, u"2 files of this track found, none better than yours (From lossless)."_s);

    // Yours is below 192: both are better, the best of them first.
    floor = selecta::core::QualityTier::Below192;
    service->find({line.id});
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    line = service->lines().constFirst();
    ASSERT_TRUE(line.best.has_value());
    EXPECT_EQ(line.alternatives.size(), 1);
}

// Near misses (files the preset turned down) are offered for an upgrade only when they are better than yours.
TEST_F(GetTracksServiceTest, AnUpgradesNearMissesAreBetterThanYours)
{
    Finding finding(*service);
    service->setTierLookup([](const QString &) { return std::optional(selecta::core::QualityTier::Below192); });
    service->addUpgrades({upgrade(u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s)});
    ASSERT_TRUE(QTest::qWaitFor([&finding] { return finding.network.sent.size() == 1; }, 3000));
    selecta::core::SearchService &search = finding.search;
    search.received(1, u"vinyl_ghost"_s, fileResponse(1, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 256, 372));
    search.received(1, u"bassbin"_s, fileResponse(1, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, 128, 372));
    ASSERT_TRUE(QTest::qWaitFor([this] { return service->findingCount() == 0; }, 3000));
    const GetLine line = service->lines().constFirst();
    EXPECT_EQ(line.findState, GetLine::FindState::NotFound);
    ASSERT_EQ(line.nearMisses.size(), 1); // the 256 is better than yours; the 128 isn't
    EXPECT_EQ(line.nearMisses.constFirst().peer, u"vinyl_ghost"_s);
    EXPECT_EQ(line.message, u"1 file of this track found, below the quality wanted."_s);
}
