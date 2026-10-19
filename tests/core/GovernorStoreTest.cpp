// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GovernorStore.h"

#include "base/Clock.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDateTime>
#include <QTimeZone>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::core::GovernorStore;
using selecta::governor::Governor;
using selecta::governor::Operation;
using selecta::governor::Refusal;
using selecta::governor::Request;

namespace {

constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

// One app session: a Governor with its store, on a shared database. Starting a new session over the same database
// is what a restart looks like.
struct Session
{
    Session(selecta::storage::Database &db, qint64 &wallMs)
        : governor(clock, {.today = [&wallMs] { return QDateTime::fromMSecsSinceEpoch(wallMs).date(); },
                           .random01 = [] { return 0.0; }})
        , store(db, governor, [&wallMs] { return wallMs; })
    {}

    ManualClock clock;
    Governor governor;
    GovernorStore store;
};

class GovernorStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    std::unique_ptr<Session> start()
    {
        auto session = std::make_unique<Session>(db, wallMs);
        EXPECT_TRUE(session->store.restore()) << db.lastError().toStdString();
        return session;
    }

    static void complete(Governor &governor, const QString &peer, qint64 bytes)
    {
        governor.downloadQueued(peer);
        governor.downloadStarted(peer);
        governor.downloadFinished(peer, bytes, true);
    }

    int count(const QString &table)
    {
        auto query = db.prepare(u"SELECT COUNT(*) FROM %1;"_s.arg(table));
        return query.step() == selecta::storage::Statement::Step::Row ? static_cast<int>(query.int64(0)) : -1;
    }

    selecta::storage::Database db;
    // Noon local time, so adding hours in a test doesn't cross midnight by accident.
    qint64 wallMs = QDateTime(QDate(2026, 10, 20), QTime(12, 0), QTimeZone::LocalTime).toMSecsSinceEpoch();
};

} // namespace

TEST_F(GovernorStoreTest, TodaysTotalsSurviveARestart)
{
    {
        auto first = start();
        complete(first->governor, u"vinyl_ghost"_s, 10'000'000);
        complete(first->governor, u"vinyl_ghost"_s, 5'000'000);
    }
    auto second = start();
    EXPECT_EQ(second->governor.peerToday(u"vinyl_ghost"_s).files, 2);
    EXPECT_EQ(second->governor.peerToday(u"vinyl_ghost"_s).bytes, 15'000'000);
}

TEST_F(GovernorStoreTest, IncompleteDownloadsAreNotSaved)
{
    {
        auto first = start();
        first->governor.downloadQueued(u"peer"_s);
        first->governor.downloadStarted(u"peer"_s);
        first->governor.downloadFinished(u"peer"_s, 4'000'000, false);
    }
    EXPECT_EQ(count(u"peer_days"_s), 0);
}

TEST_F(GovernorStoreTest, OldDaysAreForgottenButTheWeekStillCounts)
{
    {
        auto first = start();
        complete(first->governor, u"peer"_s, 1);
    }
    wallMs += 3 * kDayMs;
    {
        auto later = start();
        EXPECT_EQ(later->governor.peerToday(u"peer"_s).files, 0); // a new day
        EXPECT_EQ(count(u"peer_days"_s), 1);                      // still inside the weekly window
    }
    wallMs += 30 * kDayMs;
    const auto muchLater = start();
    EXPECT_EQ(count(u"peer_days"_s), 0);
}

TEST_F(GovernorStoreTest, BackoffSurvivesARestartUntilItEnds)
{
    {
        auto first = start();
        first->governor.peerDenied(u"strict"_s, u"Banned"_s);
    }
    wallMs += 2 * kDayMs;
    {
        auto restarted = start();
        const auto decision = restarted->governor.request(Operation::QueueDownload, Request{.peer = u"strict"_s});
        EXPECT_EQ(decision.refusal, Refusal::PeerBackoff);
    }
    wallMs += 29 * kDayMs;
    auto afterwards = start();
    EXPECT_EQ(afterwards->governor.request(Operation::QueueDownload, Request{.peer = u"strict"_s}).refusal,
              Refusal::None);
    EXPECT_EQ(count(u"peer_backoffs"_s), 0);
}

TEST_F(GovernorStoreTest, TheLongerBackoffWins)
{
    {
        auto first = start();
        first->governor.peerDenied(u"peer"_s, u"Banned"_s);
        first->governor.peerDenied(u"peer"_s, u"Pending shutdown."_s);
    }
    wallMs += kDayMs;
    auto restarted = start();
    EXPECT_EQ(restarted->governor.request(Operation::QueueDownload, Request{.peer = u"peer"_s}).refusal,
              Refusal::PeerBackoff);
}

TEST_F(GovernorStoreTest, DenialsWithoutABackoffAreNotSaved)
{
    {
        auto first = start();
        first->governor.peerDenied(u"peer"_s, u"File not shared."_s); // once is not a pattern
        first->governor.peerDenied(u"other"_s, u"Too many files"_s);
    }
    EXPECT_EQ(count(u"peer_backoffs"_s), 0);
}

TEST_F(GovernorStoreTest, LongReasonsAreTruncated)
{
    auto session = start();
    session->governor.peerDenied(u"peer"_s, u"You are banned because "_s + QString(5000, u'x'));
    auto query = db.prepare(u"SELECT length(reason) FROM peer_backoffs;");
    ASSERT_EQ(query.step(), selecta::storage::Statement::Step::Row);
    EXPECT_LE(query.int64(0), 200);
}

TEST_F(GovernorStoreTest, RestoreReportsAnUnreadableDatabase)
{
    ASSERT_TRUE(db.exec(u"DROP TABLE peer_backoffs;"));
    Session session(db, wallMs);
    EXPECT_FALSE(session.store.restore());
}
