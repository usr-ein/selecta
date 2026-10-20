// SPDX-License-Identifier: GPL-3.0-or-later
// LIB-6: the user's corrections of matches. Track identities and the store; ownership settled with verdicts; search
// results and Get tracks lines that keep a correction across a restart and a rescan of a real collection folder.
// Supersede corrections are in AlbumServiceTest.
#include "core/MatchCorrections.h"

#include "SyntheticAudio.h"
#include "base/Clock.h"
#include "convert/TagWriter.h"
#include "core/CollectionService.h"
#include "core/GetTracksService.h"
#include "core/ResultAnnotator.h"
#include "core/SearchResults.h"
#include "core/SearchService.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::core::CorrectionKind;
using selecta::core::MatchCorrectionStore;
using selecta::core::OwnedCandidate;
using selecta::core::Ownership;
using selecta::core::SearchResult;
using selecta::core::TrackIdentity;
using selecta::core::Verdict;
using selecta::match::Relation;
using selecta::match::Source;
using selecta::match::Tier;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;

namespace {

TrackIdentity track(const QString &artist, const QString &title, qint64 durationMs = 0)
{
    return selecta::core::trackIdentity(artist, title, durationMs);
}

bool writeTrack(const QString &path, const QString &artist, const QString &title)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray bytes = synth::makeMp3(synth::sine(440.0, 4.0));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return false;
    }
    file.close();
    selecta::convert::MetadataModel tags;
    tags.set(keys::Artist, artist, selecta::convert::Provenance::Embedded);
    tags.set(keys::Title, title, selecta::convert::Provenance::Embedded);
    return selecta::convert::writeTags(path, tags).ok();
}

// A search network that only counts what it is asked to search.
struct CountingNetwork final : selecta::core::SearchNetwork
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

class MatchCorrectionStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        open();
    }

    void open()
    {
        db = std::make_unique<selecta::storage::Database>();
        ASSERT_TRUE(db->open(dir.filePath(u"selecta.sqlite3"_s)));
        ASSERT_TRUE(selecta::storage::migrate(*db, selecta::storage::selectaMigrations()).ok());
    }

    QTemporaryDir dir;
    std::unique_ptr<selecta::storage::Database> db;
};

} // namespace

TEST(TrackIdentity, IsTheArtistTitleAndVersion)
{
    EXPECT_EQ(track(u"Okuma Drift"_s, u"Neon Rain"_s).key, track(u"OKUMA DRIFT"_s, u"neon rain"_s).key);
    EXPECT_NE(track(u"Okuma Drift"_s, u"Neon Rain (Extended Mix)"_s).key,
              track(u"Okuma Drift"_s, u"Neon Rain (Original Mix)"_s).key);
    EXPECT_NE(track(u"Okuma Drift"_s, u"Neon Rain"_s).key, track(u"Halden Rue"_s, u"Neon Rain"_s).key);
    EXPECT_FALSE(track(u"Okuma Drift"_s, QString()).isValid());
    EXPECT_FALSE(track(QString(), u" - ()"_s).isValid());
    EXPECT_EQ(track(u"Okuma Drift"_s, u"Neon Rain"_s, -5).durationMs, 0);

    // An Inbox item's version joins its title, as the match index has it.
    selecta::library::InboxItem item;
    item.artist = u"Okuma Drift"_s;
    item.title = u"Neon Rain"_s;
    item.version = u"Extended Mix"_s;
    item.durationMs = 372'000;
    const TrackIdentity extended = track(u"Okuma Drift"_s, u"Neon Rain (Extended Mix)"_s, 372'000);
    EXPECT_EQ(selecta::core::inboxIdentity(item), extended);
    item.title = u"Neon Rain (Extended Mix)"_s; // already in the title
    EXPECT_EQ(selecta::core::inboxIdentity(item), extended);

    // A remote file: its parse, and its length in seconds.
    const selecta::match::ParsedRemote parsed =
        selecta::match::RemotePathParser::parse(u"@@x\\Music\\Okuma Drift - Neon Rain (Extended Mix).flac"_s);
    EXPECT_EQ(selecta::core::remoteIdentity(parsed, 372), extended);
    EXPECT_FALSE(selecta::core::remoteIdentity(selecta::match::RemotePathParser::parse(u"@@x\\Music\\01.flac"_s), 300)
                     .isValid());
}

TEST_F(MatchCorrectionStoreTest, AVerdictHoldsEitherWayRoundAndTheNewestWins)
{
    MatchCorrectionStore store(*db);
    EXPECT_TRUE(store.isEmpty());
    const TrackIdentity remote = track(u"Okuma Drift"_s, u"Neon Rain (Extended Mix)"_s, 372'000);
    const TrackIdentity owned = track(u"Okuma Drift"_s, u"Neon Rain"_s, 371'000);
    EXPECT_FALSE(store.verdict(CorrectionKind::SearchResult, remote, owned).has_value());
    ASSERT_TRUE(store.record(CorrectionKind::SearchResult, remote, owned, Verdict::Different, 1000));
    EXPECT_FALSE(store.isEmpty());
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, remote, owned), Verdict::Different);
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, owned, remote), Verdict::Different);
    // The user changes their mind: one verdict per pair, the newest.
    ASSERT_TRUE(store.record(CorrectionKind::SearchResult, owned, remote, Verdict::Same, 2000));
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, remote, owned), Verdict::Same);
    ASSERT_EQ(store.all().size(), 1);
    EXPECT_EQ(store.all().constFirst().verdict, Verdict::Same);
    EXPECT_EQ(store.all().constFirst().createdAt, 2000);
    // Nothing about other pairs.
    EXPECT_FALSE(store.verdict(CorrectionKind::SearchResult, remote, track(u"Halden Rue"_s, u"Tidewater"_s)));
}

TEST_F(MatchCorrectionStoreTest, LengthsMustAgree)
{
    MatchCorrectionStore store(*db);
    const TrackIdentity owned = track(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000);
    ASSERT_TRUE(store.record(CorrectionKind::SearchResult, track(u"Okuma Drift"_s, u"Neon Rain"_s, 410'000), owned,
                             Verdict::Different, 1000));
    // Within 3 s, or unknown: the same pair.
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, track(u"Okuma Drift"_s, u"Neon Rain"_s, 412'000), owned),
              Verdict::Different);
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, track(u"Okuma Drift"_s, u"Neon Rain"_s), owned),
              Verdict::Different);
    // Another edit of the same name is another track.
    EXPECT_FALSE(store.verdict(CorrectionKind::SearchResult, track(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000), owned));
}

TEST_F(MatchCorrectionStoreTest, SupersedeVerdictsStayWithSupersede)
{
    MatchCorrectionStore store(*db);
    const TrackIdentity single = track(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000);
    const TrackIdentity onAlbum = track(u"Okuma Drift"_s, u"Neon Rain"_s, 373'000);
    ASSERT_TRUE(store.record(CorrectionKind::Supersede, single, onAlbum, Verdict::Different, 1000));
    EXPECT_EQ(store.verdict(CorrectionKind::Supersede, onAlbum, single), Verdict::Different);
    // Two tracks can share a name and a length: the single's verdict doesn't hide that copy from searches.
    EXPECT_FALSE(store.verdict(CorrectionKind::SearchResult, onAlbum, single).has_value());
    EXPECT_FALSE(store.verdict(CorrectionKind::GetLine, onAlbum, single).has_value());
    // A wanted track against an owned copy holds for search results and Get tracks lines alike.
    ASSERT_TRUE(store.record(CorrectionKind::GetLine, onAlbum, single, Verdict::Same, 2000));
    EXPECT_EQ(store.verdict(CorrectionKind::SearchResult, single, onAlbum), Verdict::Same);
    EXPECT_EQ(store.verdict(CorrectionKind::Supersede, single, onAlbum), Verdict::Different); // untouched
    EXPECT_EQ(store.all().size(), 2);
}

TEST_F(MatchCorrectionStoreTest, RefusesTracksWithoutIdentity)
{
    MatchCorrectionStore store(*db);
    EXPECT_FALSE(store.record(CorrectionKind::SearchResult, TrackIdentity{}, track(u"A"_s, u"B"_s), Verdict::Same, 1));
    EXPECT_FALSE(store.record(CorrectionKind::SearchResult, track(u"A"_s, u"B"_s), {}, Verdict::Different, 1));
    EXPECT_TRUE(store.isEmpty());
    EXPECT_FALSE(store.verdict(CorrectionKind::SearchResult, {}, {}).has_value());
}

TEST_F(MatchCorrectionStoreTest, SurvivesARestart)
{
    const TrackIdentity remote = track(u"Okuma Drift"_s, u"Neon Rain"_s, 372'000);
    const TrackIdentity owned = track(u"Okuma Drift"_s, u"Neon Rain (Live)"_s, 380'000);
    ASSERT_TRUE(MatchCorrectionStore(*db).record(CorrectionKind::SearchResult, remote, owned, Verdict::Different, 1));
    db.reset();
    open();
    EXPECT_EQ(MatchCorrectionStore(*db).verdict(CorrectionKind::SearchResult, remote, owned), Verdict::Different);
}

TEST(SettleOwnership, VerdictsSetCandidatesAsideOrMakeThemSure)
{
    SearchResult result;
    result.candidates = {OwnedCandidate{1, Source::Collection, Tier::Likely, Relation::Same},
                         OwnedCandidate{2, Source::Inbox, Tier::Exact, Relation::Same},
                         OwnedCandidate{3, Source::Collection, Tier::Exact, Relation::OtherVersion}};
    const auto with = [&result](const QHash<quint64, Verdict> &verdicts) {
        selecta::core::settleOwnership(result, [verdicts](quint64 id, Source /*source*/) {
            return verdicts.contains(id) ? std::optional<Verdict>(verdicts.value(id)) : std::nullopt;
        });
        return std::pair{result.ownership, result.ownedId};
    };
    EXPECT_EQ(with({}), std::pair(Ownership::LikelyInCollection, quint64{1}));
    EXPECT_EQ(with({{1, Verdict::Same}}), std::pair(Ownership::InCollection, quint64{1}));
    EXPECT_EQ(with({{1, Verdict::Different}}), std::pair(Ownership::InInbox, quint64{2}));
    EXPECT_EQ(with({{1, Verdict::Different}, {2, Verdict::Different}}), std::pair(Ownership::OtherVersion, quint64{3}));
    EXPECT_EQ(with({{1, Verdict::Different}, {2, Verdict::Different}, {3, Verdict::Different}}),
              std::pair(Ownership::None, quint64{0}));
    // Another version the user says is the same track is a sure match, ahead of the Inbox.
    EXPECT_EQ(with({{1, Verdict::Different}, {3, Verdict::Same}}), std::pair(Ownership::InCollection, quint64{3}));
    // A past download of this very file comes before another version.
    result.historyIds = {9};
    EXPECT_EQ(with({{1, Verdict::Different}, {2, Verdict::Different}}),
              std::pair(Ownership::DownloadedBefore, quint64{0}));
    // A better copy stays an upgrade once confirmed.
    result.candidates = {OwnedCandidate{4, Source::Collection, Tier::Likely, Relation::Upgrade}};
    EXPECT_EQ(with({{4, Verdict::Same}}), std::pair(Ownership::Upgrade, quint64{4}));
}

namespace {

// A real collection folder, scanned into a real index, with a database that can be closed and opened again.
class LibraryCorrectionsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        root = dir.filePath(u"Music"_s);
        ASSERT_TRUE(writeTrack(root + u"/Singles/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s));
        ASSERT_TRUE(writeTrack(root + u"/Singles/Halden Rue - Tidewater.mp3"_s, u"Halden Rue"_s, u"Tidewater"_s));
        start();
    }

    // Opens the database and scans the collection: the first launch, or the next one.
    void start()
    {
        db = std::make_unique<selecta::storage::Database>();
        ASSERT_TRUE(db->open(dir.filePath(u"selecta.sqlite3"_s)));
        ASSERT_TRUE(selecta::storage::migrate(*db, selecta::storage::selectaMigrations()).ok());
        collection = std::make_unique<selecta::core::CollectionService>(
            *db, selecta::core::CollectionService::Options{.readers = 2, .watch = false, .rebuildDelayMs = 10},
            [] { return qint64{1'800'000'000'000}; });
        collection->setRoots({root}, {});
        collection->waitForIdle();
        ASSERT_NE(collection->index(), nullptr);
    }

    void restart()
    {
        collection.reset();
        db.reset();
        start();
    }

    // A rescan that finds the file changed (new tags written), so it is read and indexed again.
    void rescan()
    {
        ASSERT_TRUE(writeTrack(root + u"/Singles/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s));
        const quint64 before = collection->index()->generation();
        collection->scanNow();
        collection->waitForIdle();
        ASSERT_GT(collection->index()->generation(), before);
    }

    // What a search for these files shows, annotated as the engine does.
    QList<SearchResult> search(const QList<std::pair<QString, int>> &files)
    {
        QList<selecta::core::SearchHit> hits;
        for (const auto &[path, seconds] : files) {
            selecta::core::SearchHit hit;
            hit.peer = u"vinyl_ghost"_s;
            hit.path = path.toUtf8();
            hit.extension = QFileInfo(path).suffix();
            hit.size = 10'000'000;
            if (seconds > 0) {
                hit.attributes.duration = seconds;
            }
            hits.append(hit);
        }
        QList<SearchResult> results = selecta::core::analyzeHits(hits, 0, collection->index(), std::nullopt);
        selecta::core::ResultAnnotator annotator(*db, governor, {}, std::nullopt);
        for (SearchResult &result : results) {
            annotator.annotate(result);
        }
        return results;
    }

    // What Engine::correctSearchMatch() does.
    void correct(const SearchResult &result, Verdict verdict)
    {
        ASSERT_TRUE(selecta::core::ResultAnnotator::recordCorrection(*db, result, verdict, 1000));
    }

    QTemporaryDir dir;
    QString root;
    std::unique_ptr<selecta::storage::Database> db;
    std::unique_ptr<selecta::core::CollectionService> collection;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
};

const QString kNeonRain = u"@@vinyl\\Music\\Okuma Drift - Neon Rain.mp3"_s;
const QString kTidewater = u"@@vinyl\\Music\\Halden Rue - Tidewater.mp3"_s;

} // namespace

TEST_F(LibraryCorrectionsTest, NotWhatIHaveSurvivesARestartAndARescan)
{
    const QList<SearchResult> results = search({{kNeonRain, 4}, {kTidewater, 4}});
    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results.at(0).ownership, Ownership::InCollection);
    // A result that owns nothing has nothing to correct.
    EXPECT_FALSE(selecta::core::ResultAnnotator::recordCorrection(*db, SearchResult{}, Verdict::Different, 1000));
    correct(results.at(0), Verdict::Different);

    const auto check = [this] {
        const QList<SearchResult> again = search({{kNeonRain, 4}, {kTidewater, 4}});
        ASSERT_EQ(again.size(), 2);
        EXPECT_EQ(again.at(0).ownership, Ownership::None);
        EXPECT_EQ(again.at(0).ownedId, 0U);
        EXPECT_FALSE(again.at(0).candidates.isEmpty()); // the matcher still found it; the user's verdict sets it aside
        EXPECT_EQ(again.at(1).ownership, Ownership::InCollection); // the other track is untouched
    };
    check();
    restart();
    check();
    rescan();
    check();
}

TEST_F(LibraryCorrectionsTest, SameAsWhatIHaveMakesALikelyMatchSure)
{
    // No length announced: a likely match at best.
    const QList<SearchResult> results = search({{kNeonRain, 0}});
    ASSERT_EQ(results.at(0).ownership, Ownership::LikelyInCollection);
    correct(results.at(0), Verdict::Same);
    EXPECT_EQ(search({{kNeonRain, 0}}).at(0).ownership, Ownership::InCollection);
    restart();
    rescan();
    EXPECT_EQ(search({{kNeonRain, 0}}).at(0).ownership, Ownership::InCollection);
}

TEST_F(LibraryCorrectionsTest, NotWhatIHaveFindsAGetTracksLine)
{
    const selecta::base::ManualClock searchClock;
    selecta::governor::Governor searchGovernor(searchClock);
    CountingNetwork network;
    selecta::core::SearchService searchService(
        searchGovernor, network, searchClock,
        {.idle = std::chrono::milliseconds(40), .total = std::chrono::milliseconds(1000)});
    auto getTracks = std::make_unique<selecta::core::GetTracksService>(*db, [] { return qint64{1'800'000'000'000}; });
    getTracks->setSearch(&searchService, [] { return QStringList(); });
    getTracks->add(u"Okuma Drift - Neon Rain\nHalden Rue - Tidewater"_s);
    getTracks->setLibraryIndex(collection->index());
    using FindState = selecta::core::GetLine::FindState;
    ASSERT_EQ(getTracks->lines().at(0).findState, FindState::Owned);
    ASSERT_EQ(getTracks->lines().at(1).findState, FindState::Owned);
    const qint64 neonRain = getTracks->lines().at(0).id;

    // "Not What I Have, Find It": remembered, and searched for.
    getTracks->notOwned({neonRain});
    EXPECT_NE(getTracks->lines().at(0).findState, FindState::Owned);
    EXPECT_EQ(getTracks->lines().at(1).findState, FindState::Owned);
    EXPECT_TRUE(QTest::qWaitFor([&network] { return network.sent.size() == 1; }, 3000));
    EXPECT_TRUE(network.sent.constFirst().contains(u"neon rain"_s, Qt::CaseInsensitive))
        << network.sent.constFirst().toStdString();
    // It holds for search results too: the same wanted track against the same copy.
    EXPECT_EQ(search({{kNeonRain, 4}}).at(0).ownership, Ownership::None);

    // After a restart and a rescan the line still isn't "You have it".
    getTracks.reset();
    restart();
    rescan();
    selecta::core::GetTracksService restarted(*db, [] { return qint64{1'800'000'000'000}; });
    restarted.setLibraryIndex(collection->index());
    EXPECT_NE(restarted.lines().at(0).findState, FindState::Owned);
    EXPECT_EQ(restarted.lines().at(1).findState, FindState::Owned);
}
