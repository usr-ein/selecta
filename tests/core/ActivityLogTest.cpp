// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ActivityLog.h"

#include "base/Clock.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::ActivityEntry;
using selecta::core::ActivityLog;

namespace {

constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

class ActivityLogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    int count()
    {
        auto query = db.prepare(u"SELECT COUNT(*) FROM activity;");
        return query.step() == selecta::storage::Statement::Step::Row ? static_cast<int>(query.int64(0)) : -1;
    }

    selecta::storage::Database db;
    qint64 nowMs = 1'800'000'000'000;
    ActivityLog log{db, [this] { return nowMs; }};
};

} // namespace

TEST_F(ActivityLogTest, RecordsAndListsNewestFirst)
{
    const auto first = log.record(u"search"_s, u"neon rain"_s, u"allowed"_s, u"Search sent."_s);
    nowMs += 1000;
    const auto second = log.record(u"server"_s, QString(), u"connected"_s, u"Logged in."_s);
    ASSERT_GT(first.id, 0);
    ASSERT_GT(second.id, first.id);

    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].id, second.id);
    EXPECT_EQ(entries[0].operation, u"server"_s);
    EXPECT_EQ(entries[1].subject, u"neon rain"_s);
    EXPECT_EQ(entries[1].atMs, nowMs - 1000);
    EXPECT_EQ(ActivityLog::recent(db, 1).size(), 1);
    EXPECT_TRUE(ActivityLog::recent(db, -5).isEmpty());
}

TEST_F(ActivityLogTest, EmitsEachLineWithItsId)
{
    const QSignalSpy spy(&log, &ActivityLog::recorded);
    const auto entry = log.record(u"browse"_s, u"vinyl_ghost"_s, u"allowed"_s, QString());
    ASSERT_EQ(spy.size(), 1);
    EXPECT_EQ(spy.at(0).at(0).value<ActivityEntry>().id, entry.id);
}

TEST_F(ActivityLogTest, RecordsGovernorDecisions)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);

    governor.request(selecta::governor::Operation::Search, {.query = u"okuma drift"_s});
    governor.setPaused(true);
    governor.request(selecta::governor::Operation::Browse, {.peer = u"vinyl_ghost"_s});

    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[1].operation, u"search"_s);
    EXPECT_EQ(entries[1].outcome, u"allowed"_s);
    EXPECT_EQ(entries[0].operation, u"browse"_s);
    EXPECT_EQ(entries[0].subject, u"vinyl_ghost"_s);
    EXPECT_EQ(entries[0].outcome, u"network-paused"_s);
    EXPECT_FALSE(entries[0].explanation.isEmpty());
}

TEST_F(ActivityLogTest, PrivateMessageTextIsNeverLogged)
{
    const selecta::base::ManualClock clock;
    selecta::governor::Governor governor(clock);
    QObject::connect(&governor, &selecta::governor::Governor::decided, &log, &ActivityLog::recordDecision);
    governor.request(selecta::governor::Operation::PrivateMessage,
                     {.recipient = u"friend"_s, .text = u"my secret plans"_s});
    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].subject, u"friend"_s);
    EXPECT_FALSE(entries[0].explanation.contains(u"secret"_s));
}

TEST_F(ActivityLogTest, PrunesByAge)
{
    log.record(u"search"_s, u"old"_s, u"allowed"_s, QString());
    nowMs += (ActivityLog::kKeepDays + 1) * kDayMs;
    log.record(u"search"_s, u"new"_s, u"allowed"_s, QString());
    EXPECT_EQ(log.prune(), 1);
    const auto entries = ActivityLog::recent(db, 10);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].subject, u"new"_s);
}

TEST_F(ActivityLogTest, PrunesToTheNewestRows)
{
    // Bulk-insert past the cap in one statement; the newest rows (highest ids) must be the ones kept.
    auto fill = db.prepare(u"WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM n WHERE x < ?1) "
                           u"INSERT INTO activity (at, operation, subject, outcome) "
                           u"SELECT ?2, 'search', CAST(x AS TEXT), 'allowed' FROM n;");
    ASSERT_TRUE(fill.bind(1, ActivityLog::kMaxRows + 25).bind(2, nowMs).run()) << db.lastError().toStdString();
    EXPECT_EQ(log.prune(), 25);
    EXPECT_EQ(count(), ActivityLog::kMaxRows);
    EXPECT_EQ(ActivityLog::recent(db, 1).constFirst().subject, QString::number(ActivityLog::kMaxRows + 25));
    EXPECT_EQ(log.prune(), 0);
}

TEST_F(ActivityLogTest, AFailedWriteIsReportedNotThrown)
{
    ASSERT_TRUE(db.exec(u"DROP TABLE activity;"));
    const QSignalSpy spy(&log, &ActivityLog::recorded);
    EXPECT_EQ(log.record(u"search"_s, u"x"_s, u"allowed"_s, QString()).id, 0);
    EXPECT_EQ(spy.size(), 0);
    EXPECT_EQ(log.prune(), -1);
}
