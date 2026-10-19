// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/Migrations.h"

#include "storage/Database.h"

#include <array>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::storage::Database;
using selecta::storage::Migration;
using selecta::storage::MigrationStatus;
using selecta::storage::Statement;

namespace {

class MigrationsTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(db.open(u":memory:"_s)); }

    bool tableExists(const QString &name)
    {
        Statement query = db.prepare(u"SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?1;");
        return query.bind(1, name).step() == Statement::Step::Row;
    }

    Database db;
};

constexpr std::array kTwo{
    Migration{1, "CREATE TABLE a (x INTEGER) STRICT;"},
    Migration{2, "CREATE TABLE b (y INTEGER) STRICT; INSERT INTO a VALUES (1);"},
};

} // namespace

TEST_F(MigrationsTest, AppliesAllInOrderAndIsIdempotent)
{
    auto result = selecta::storage::migrate(db, kTwo);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    EXPECT_EQ(result.version, 2);
    EXPECT_EQ(db.userVersion(), 2);
    EXPECT_TRUE(tableExists(u"a"_s));
    EXPECT_TRUE(tableExists(u"b"_s));

    result = selecta::storage::migrate(db, kTwo);
    ASSERT_TRUE(result.ok());
    Statement count = db.prepare(u"SELECT COUNT(*) FROM a;");
    ASSERT_EQ(count.step(), Statement::Step::Row);
    EXPECT_EQ(count.int64(0), 1); // migration 2 didn't run twice
}

TEST_F(MigrationsTest, ResumesFromTheCurrentVersion)
{
    ASSERT_TRUE(selecta::storage::migrate(db, std::span(kTwo).first(1)).ok());
    EXPECT_EQ(db.userVersion(), 1);
    ASSERT_TRUE(selecta::storage::migrate(db, kTwo).ok());
    EXPECT_EQ(db.userVersion(), 2);
}

TEST_F(MigrationsTest, FailedMigrationRollsBackAndStops)
{
    constexpr std::array broken{
        Migration{1, "CREATE TABLE a (x INTEGER) STRICT;"},
        Migration{2, "CREATE TABLE b (y INTEGER) STRICT; INSERT INTO missing VALUES (1);"},
        Migration{3, "CREATE TABLE c (z INTEGER) STRICT;"},
    };
    const auto result = selecta::storage::migrate(db, broken);
    EXPECT_EQ(result.status, MigrationStatus::Failed);
    EXPECT_EQ(result.version, 1);
    EXPECT_EQ(db.userVersion(), 1);
    EXPECT_FALSE(tableExists(u"b"_s)); // migration 2 rolled back as a whole
    EXPECT_FALSE(tableExists(u"c"_s));
    EXPECT_TRUE(result.error.contains(u"migration 2"_s));
}

TEST_F(MigrationsTest, RefusesADatabaseFromANewerVersion)
{
    ASSERT_TRUE(db.setUserVersion(5));
    const auto result = selecta::storage::migrate(db, kTwo);
    EXPECT_EQ(result.status, MigrationStatus::DatabaseTooNew);
    EXPECT_EQ(db.userVersion(), 5);
}

TEST_F(MigrationsTest, RefusesAnInvalidList)
{
    constexpr std::array gap{Migration{1, "SELECT 1;"}, Migration{3, "SELECT 1;"}};
    constexpr std::array zero{Migration{0, "SELECT 1;"}};
    constexpr std::array nullSql{Migration{1, nullptr}};
    EXPECT_EQ(selecta::storage::migrate(db, gap).status, MigrationStatus::InvalidList);
    EXPECT_EQ(selecta::storage::migrate(db, zero).status, MigrationStatus::InvalidList);
    EXPECT_EQ(selecta::storage::migrate(db, nullSql).status, MigrationStatus::InvalidList);
    EXPECT_EQ(db.userVersion(), 0);
}

TEST_F(MigrationsTest, SelectasOwnHistoryApplies)
{
    const auto result = selecta::storage::migrate(db, selecta::storage::selectaMigrations());
    ASSERT_TRUE(result.ok()) << result.error.toStdString();
    EXPECT_EQ(result.version, static_cast<int>(selecta::storage::selectaMigrations().size()));
    EXPECT_TRUE(tableExists(u"sessions"_s));
}
