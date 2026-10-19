// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "core/EngineHost.h"
#include "storage/Database.h"

#include <QSignalSpy>
#include <QTemporaryDir>
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
