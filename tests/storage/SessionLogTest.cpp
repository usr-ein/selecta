// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/SessionLog.h"

#include "storage/Database.h"
#include "storage/Migrations.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::storage::Database;
using selecta::storage::SessionLog;

namespace {

class SessionLogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    Database db;
};

} // namespace

TEST_F(SessionLogTest, NumbersSessionsFromOne)
{
    SessionLog log(db);
    EXPECT_EQ(log.latest(), 0);
    EXPECT_EQ(log.start(1000), 1);
    EXPECT_EQ(log.start(2000), 2);
    EXPECT_EQ(log.latest(), 2);
}

TEST_F(SessionLogTest, EndsASessionOnce)
{
    SessionLog log(db);
    const int session = log.start(1000);
    EXPECT_TRUE(log.end(session, 5000));
    EXPECT_FALSE(log.end(session, 6000)); // already ended
    EXPECT_FALSE(log.end(99, 6000));      // no such session
}

TEST_F(SessionLogTest, ListsRecentSessionsNewestFirst)
{
    SessionLog log(db);
    const int first = log.start(1000);
    ASSERT_TRUE(log.end(first, 4000));
    log.start(9000); // still running

    const auto sessions = log.recent(10);
    ASSERT_EQ(sessions.size(), 2);
    EXPECT_EQ(sessions[0].number, 2);
    EXPECT_EQ(sessions[0].startedAtMs, 9000);
    EXPECT_FALSE(sessions[0].endedAtMs.has_value());
    EXPECT_EQ(sessions[1].number, 1);
    EXPECT_EQ(sessions[1].endedAtMs, 4000);
    EXPECT_EQ(log.recent(1).size(), 1);
    EXPECT_TRUE(log.recent(0).isEmpty());
}
