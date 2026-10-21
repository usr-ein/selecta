// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "core/EngineHost.h"
#include "core/InboxService.h"
#include "core/ShareService.h"
#include "slsk/PeerMessages.h"
#include "storage/Database.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::ActivityEntry;
using selecta::core::Engine;
using selecta::core::EngineHost;
using selecta::core::EngineOptions;
using selecta::governor::Operation;
using selecta::governor::Refusal;

namespace {

class EngineTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }

    EngineOptions options() const
    {
        EngineOptions result;
        result.databasePath = dir.filePath(u"selecta.sqlite3"_s);
        result.clock = &clock;
        result.wallClock = [] { return qint64{1'800'000'000'000}; };
        result.metaEndpoints = selecta::core::MetaEndpoints::unreachable(); // tests never reach real services
        return result;
    }

    QTemporaryDir dir;
    selecta::base::ManualClock clock;
};

} // namespace

TEST_F(EngineTest, StartsOnAnEmptyDatabaseAndRecordsDecisions)
{
    Engine engine(options());
    const QSignalSpy finished(&engine, &Engine::startFinished);
    ASSERT_TRUE(engine.start()) << engine.error().toStdString();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(0).toBool());

    const auto decision = engine.governor().request(Operation::Search, {.query = u"okuma drift"_s});
    EXPECT_TRUE(decision.allowed());
    const auto lines = selecta::core::ActivityLog::recent(engine.database(), 10);
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0].operation, u"search"_s);
}

TEST_F(EngineTest, TheSearchBudgetIsReportedOnceAndAgainOnlyWhenItRefills)
{
    // With searches spent but some left, the engine waits for the next refill (the clock here stands still): it
    // must not spin re-reporting the same budget.
    Engine engine(options());
    ASSERT_TRUE(engine.start()) << engine.error().toStdString();
    ASSERT_TRUE(engine.governor().request(Operation::Search, {.query = u"okuma drift"_s}).allowed());
    ASSERT_TRUE(engine.governor().request(Operation::Search, {.query = u"halden rue"_s}).allowed());
    const QSignalSpy reports(&engine, &Engine::searchBudgetChanged);
    QTest::qWait(200);
    EXPECT_LE(reports.size(), 2);
}

TEST_F(EngineTest, PauseRefusesSoulseekOperationsAndIsLogged)
{
    Engine engine(options());
    ASSERT_TRUE(engine.start());
    const QSignalSpy paused(&engine, &Engine::pausedChanged);
    engine.setPaused(true);
    engine.setPaused(true); // no change, no second signal
    ASSERT_EQ(paused.size(), 1);
    EXPECT_TRUE(engine.isPaused());
    EXPECT_EQ(engine.governor().request(Operation::Search, {.query = u"neon rain"_s}).refusal, Refusal::NetworkPaused);
    const auto lines = selecta::core::ActivityLog::recent(engine.database(), 10);
    ASSERT_GE(lines.size(), 2);
    EXPECT_EQ(lines.last().outcome, u"paused"_s);
}

TEST_F(EngineTest, StartsPausedWhenAskedTo)
{
    EngineOptions paused = options();
    paused.startPaused = true;
    Engine engine(paused);
    ASSERT_TRUE(engine.start());
    EXPECT_TRUE(engine.isPaused());
}

TEST_F(EngineTest, PeerLimitsSurviveARestart)
{
    {
        Engine first(options());
        ASSERT_TRUE(first.start());
        first.governor().peerDenied(u"strict"_s, u"Banned"_s);
    }
    Engine second(options());
    ASSERT_TRUE(second.start());
    EXPECT_EQ(second.governor().request(Operation::QueueDownload, {.peer = u"strict"_s}).refusal, Refusal::PeerBackoff);
}

TEST_F(EngineTest, ReportsADatabaseItCantOpen)
{
    EngineOptions broken = options();
    broken.databasePath = dir.filePath(u"missing/folder/selecta.sqlite3"_s);
    Engine engine(broken);
    const QSignalSpy finished(&engine, &Engine::startFinished);
    EXPECT_FALSE(engine.start());
    EXPECT_FALSE(engine.error().isEmpty());
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(0).toBool());
    EXPECT_FALSE(engine.isStarted());
    engine.stop(); // idempotent
}

TEST_F(EngineTest, HostRunsTheEngineOnItsOwnThread)
{
    EngineHost host(options());
    QSignalSpy started(&host, &EngineHost::started);
    QSignalSpy paused(&host, &EngineHost::pausedChanged);
    QSignalSpy activity(&host, &EngineHost::activityRecorded);
    host.start();
    ASSERT_TRUE(started.wait(5000));
    EXPECT_TRUE(started.at(0).at(0).toBool());
    EXPECT_TRUE(host.isRunning());

    QThread *engineThread = nullptr;
    host.post([&engineThread](Engine &engine) { engineThread = engine.thread(); });
    host.setPaused(true);
    ASSERT_TRUE(paused.wait(5000));
    EXPECT_TRUE(host.isPaused());
    ASSERT_TRUE(activity.wait(5000) || !activity.isEmpty());
    EXPECT_EQ(activity.at(0).at(0).value<ActivityEntry>().outcome, u"paused"_s);
    EXPECT_NE(engineThread, nullptr);
    EXPECT_NE(engineThread, QThread::currentThread());

    host.stop();
    EXPECT_FALSE(host.isRunning());
    host.stop(); // safe twice
}

TEST_F(EngineTest, HostReportsAFailedStart)
{
    EngineOptions broken = options();
    broken.databasePath = dir.filePath(u"missing/folder/selecta.sqlite3"_s);
    EngineHost host(broken);
    QSignalSpy started(&host, &EngineHost::started);
    host.start();
    ASSERT_TRUE(started.wait(5000));
    EXPECT_FALSE(started.at(0).at(0).toBool());
    EXPECT_FALSE(started.at(0).at(1).toString().isEmpty());
}

TEST_F(EngineTest, HostStopsCleanlyBeforeTheEngineStarted)
{
    // Destroying the host straight after start() must not deadlock or leak the engine.
    EngineHost host(options());
    host.start();
}

TEST_F(EngineTest, PreviewsFallBackFromYouTubeToTheClipToThePeer)
{
    EngineOptions opts = options();
    opts.previewDir = dir.filePath(u"previews"_s);
    Engine engine(opts);
    ASSERT_TRUE(engine.start()) << engine.error().toStdString();
    engine.addTracks(u"Okuma Drift - Neon Rain"_s);
    const QList<selecta::core::GetLine> lines = engine.getTracks().lines();
    ASSERT_EQ(lines.size(), 1);
    const qint64 lineId = lines.constFirst().id;

    // Isolated: no yt-dlp, no Deezer, and this line has no file on Soulseek. Each source says why, the last one wins.
    QSignalSpy refused(&engine, &Engine::previewRefused);
    QSignalSpy changes(&engine, &Engine::previewChanged);
    engine.previewLine(lineId, selecta::core::PreviewSource::Automatic);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !refused.isEmpty(); }, 5000));
    EXPECT_EQ(refused.at(0).at(0).toString(), selecta::core::previewKeyForLine(lineId));
    EXPECT_FALSE(refused.at(0).at(1).toString().isEmpty());
    bool triedYouTube = false;
    bool triedClip = false;
    for (const QList<QVariant> &change : std::as_const(changes)) {
        const auto preview = change.at(0).value<selecta::core::Preview>();
        triedYouTube = triedYouTube || preview.peer == u"YouTube"_s;
        triedClip = triedClip || preview.peer == u"Deezer"_s;
    }
    EXPECT_TRUE(triedYouTube);
    EXPECT_TRUE(triedClip);

    // Asking for one source only tries that one.
    refused.clear();
    changes.clear();
    engine.previewLine(lineId, selecta::core::PreviewSource::Peer);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !refused.isEmpty(); }, 5000));
    EXPECT_TRUE(changes.isEmpty());

    // A preview stopped meanwhile doesn't come back through a late fallback.
    refused.clear();
    engine.previewLine(lineId, selecta::core::PreviewSource::Automatic);
    engine.stopPreview();
    QTest::qWait(300);
    EXPECT_TRUE(refused.isEmpty());
}

namespace {

selecta::meta::Release releaseWithMbid()
{
    selecta::meta::Release release;
    release.title = u"Neon Rain"_s;
    release.musicBrainzReleaseId = u"c6a53877-7733-4bdb-9721-0dff076ce2ef"_s;
    return release;
}

} // namespace

TEST_F(EngineTest, ACoverThatCantBeFetchedIsAnsweredWithNone)
{
    Engine engine(options()); // cover downloads off, as in isolated runs
    const QSignalSpy fetched(&engine, &Engine::coverFetched);
    engine.fetchCover(u"before start"_s, releaseWithMbid());
    ASSERT_EQ(fetched.size(), 1);
    EXPECT_TRUE(fetched.at(0).at(1).toByteArray().isEmpty());

    ASSERT_TRUE(engine.start()) << engine.error().toStdString();
    engine.fetchCover(u"off"_s, releaseWithMbid());
    ASSERT_EQ(fetched.size(), 2);
    EXPECT_EQ(fetched.at(1).at(0).toString(), u"off"_s);
    EXPECT_TRUE(fetched.at(1).at(1).toByteArray().isEmpty());

    // Downloads on, but the archive can't be reached: no cover, said once. (Fetching one is CoverFetcher's tests'.)
    EngineOptions opts = options();
    opts.metaEndpoints.coverDownloads = true;
    opts.databasePath = dir.filePath(u"other.sqlite3"_s);
    Engine reaching(opts);
    ASSERT_TRUE(reaching.start()) << reaching.error().toStdString();
    QSignalSpy failed(&reaching, &Engine::coverFetched);
    reaching.fetchCover(u"closed"_s, releaseWithMbid());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !failed.isEmpty(); }, 5000));
    QTest::qWait(100);
    ASSERT_EQ(failed.size(), 1);
    EXPECT_EQ(failed.at(0).at(0).toString(), u"closed"_s);
    EXPECT_TRUE(failed.at(0).at(1).toByteArray().isEmpty());
}

TEST_F(EngineTest, HostForwardsCovers)
{
    EngineHost host(options());
    QSignalSpy started(&host, &EngineHost::started);
    QSignalSpy fetched(&host, &EngineHost::coverFetched);
    host.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !started.isEmpty(); }, 5000));
    host.fetchCover(u"get:host"_s, releaseWithMbid());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !fetched.isEmpty(); }, 5000));
    EXPECT_EQ(fetched.at(0).at(0).toString(), u"get:host"_s);
    EXPECT_TRUE(fetched.at(0).at(1).toByteArray().isEmpty());
}

// PRD SHARE-1: the engine shares the collection's two folders by their roles, and never Selecta's Inbox.
TEST_F(EngineTest, SharesTheCollectionsTracksByTheirFoldersRolesAndNeverTheInbox)
{
    for (const QString &relative :
         {u"Tracks/Loose.mp3"_s, u"Tracks/Sub/Deeper.mp3"_s, u"Tracks/cover.jpg"_s, u"Albums/Loose.mp3"_s,
          u"Albums/Okuma Drift - Neon Rain/01 Neon Rain.mp3"_s, u"Albums/Okuma Drift - Neon Rain/CD2/01 Bonus.mp3"_s,
          u"Albums/Okuma Drift - Neon Rain/cover.jpg"_s, u"Inbox/Singles/Waiting.mp3"_s,
          u"Inbox/Albums/Kel Varo - Amber Engine/01 Waiting.mp3"_s}) {
        ASSERT_TRUE(QDir().mkpath(QFileInfo(dir.filePath(relative)).path()));
        QFile file(dir.filePath(relative));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("bytes"), 5);
    }
    Engine engine(options());
    ASSERT_TRUE(engine.start()) << engine.error().toStdString();
    selecta::core::LibraryConfig config;
    config.inboxRoot = dir.filePath(u"Inbox"_s);
    config.tracksRoot = dir.filePath(u"Tracks"_s);
    config.albumsRoot = dir.filePath(u"Albums"_s);
    ASSERT_TRUE(selecta::core::checkLibraryConfig(config).isEmpty());
    engine.setLibraryConfig(config);
    engine.shares().waitForIdle();

    const auto decoded = selecta::slsk::peer::SharedFileListResponse::decode(engine.shares().index()->sharedFileList());
    ASSERT_TRUE(decoded.has_value());
    QStringList shared;
    for (const selecta::slsk::SharedDirectory &directory : decoded->list.directories) {
        for (const selecta::slsk::FileEntry &file : directory.files) {
            shared.append(directory.path.display() + u'\\' + file.name.display());
        }
    }
    EXPECT_EQ(shared, QStringList({u"Tracks\\Loose.mp3"_s, u"Albums\\Okuma Drift - Neon Rain\\01 Neon Rain.mp3"_s}));
    EXPECT_EQ(engine.shares().status().roots, QStringList({config.tracksRoot, config.albumsRoot}));
}
