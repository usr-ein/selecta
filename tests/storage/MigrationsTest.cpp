// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/Migrations.h"

#include "storage/Database.h"

#include <QStringList>

#include <array>
#include <cstddef>
#include <gtest/gtest.h>
#include <span>

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

    // The rows `sql` selects: one string per row, its columns joined by '|', NULL as "null".
    QStringList rows(const QString &sql)
    {
        QStringList result;
        Statement query = db.prepare(sql);
        while (query.step() == Statement::Step::Row) {
            QStringList columns;
            for (int i = 0; i < query.columnCount(); ++i) {
                columns.append(query.isNull(i) ? u"null"_s : query.text(i));
            }
            result.append(columns.join(u'|'));
        }
        return result;
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

// P-001 (PRD D39): the Inbox is Selecta's own and starts empty. Updating a database of an earlier build drops its
// Inbox items, but History keeps what it says about them.
TEST_F(MigrationsTest, TheInboxMoveForgetsTheOldItemsButNotWhatHistorySays)
{
    const std::span<const Migration> all = selecta::storage::selectaMigrations();
    constexpr auto kBeforeTheMove = static_cast<std::size_t>(selecta::storage::kInboxMovedVersion - 1);
    ASSERT_GT(all.size(), kBeforeTheMove);
    // kInboxMovedVersion names the migration that forgets the items (it moves along if the migration is renumbered).
    ASSERT_TRUE(QString::fromUtf8(all[kBeforeTheMove].sql).contains(u"DELETE FROM inbox_items"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, all.first(kBeforeTheMove)).ok()); // the schema of earlier builds
    ASSERT_TRUE(db.exec(uR"sql(
        INSERT INTO inbox_items (id, kind, relative_path, state, created_at) VALUES
            (1, 'single', 'Singles/Okuma Drift - Neon Rain.mp3', 'ready', 1),
            (2, 'album', 'Albums/Okuma Drift - Nightfall (2024)', 'ready', 2);
        INSERT INTO history (id, completed_at, kind, peer, remote_path, size_bytes, inbox_item, whereabouts, filed_path)
        VALUES
            (1, 1, 'single', 'vinyl_ghost', X'61', 5, 1, 'inbox', NULL),
            (2, 2, 'album', 'vinyl_ghost', X'62', 9, 2, 'inbox', NULL),
            (3, 3, 'single', 'vinyl_ghost', X'63', 7, NULL, 'filed', '/Music/Tracks/Marlo Vance - Tidal Memory.mp3');
        INSERT INTO album_downloads (id, peer, folder, state, inbox_item, created_at, updated_at) VALUES
            (1, 'vinyl_ghost', X'62', 'arrived', 2, 2, 2);
        INSERT INTO downloads (id, peer, remote_path, state, inbox_item, created_at, updated_at) VALUES
            (1, 'vinyl_ghost', X'61', 'arrived', 1, 1, 1);
        INSERT INTO supersede_flags (id, album, album_item, single_item, single_path, single, created_at) VALUES
            (1, 'Nightfall', 2, 1, NULL, 'Okuma Drift – Neon Rain', 1),
            (2, 'Nightfall', 2, NULL, '/Music/Tracks/Okuma Drift - Neon Rain.mp3', 'Okuma Drift – Neon Rain', 2);
    )sql"_s))
        << db.lastError().toStdString();

    const auto result = selecta::storage::migrate(db, all);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();

    EXPECT_TRUE(rows(u"SELECT id FROM inbox_items;"_s).isEmpty()); // the Inbox starts empty
    // History says what it said ("In the Inbox", "Filed: …"): only its links to the forgotten items go.
    EXPECT_EQ(rows(u"SELECT id, whereabouts, filed_path, inbox_item FROM history ORDER BY id;"_s),
              (QStringList{u"1|inbox|null|null"_s, u"2|inbox|null|null"_s,
                           u"3|filed|/Music/Tracks/Marlo Vance - Tidal Memory.mp3|null"_s}));
    EXPECT_EQ(rows(u"SELECT inbox_item FROM downloads;"_s), QStringList{u"null"_s});
    EXPECT_EQ(rows(u"SELECT inbox_item FROM album_downloads;"_s), QStringList{u"null"_s});
    // A dropped Inbox single's flag goes with it; a filed single's flag stays.
    EXPECT_EQ(rows(u"SELECT id, album_item FROM supersede_flags;"_s), QStringList{u"2|null"_s});
}

// P-010: genres join the collection index, so every file known is read once more (its signature no longer matches the
// disk), keeping its row and id; Get tracks lines, Inbox items and History get empty upgrade marks and no verdict.
TEST_F(MigrationsTest, TheUpgradesMigrationReadsEveryCollectionFileAgain)
{
    const std::span<const Migration> all = selecta::storage::selectaMigrations();
    constexpr auto kBefore = static_cast<std::size_t>(selecta::storage::kCollectionUpgradesVersion - 1);
    ASSERT_GT(all.size(), kBefore);
    ASSERT_TRUE(QString::fromUtf8(all[kBefore].sql).contains(u"upgrade_of"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, all.first(kBefore)).ok());
    ASSERT_TRUE(db.exec(uR"sql(
        INSERT INTO collection_files (id, path, path_key, size, modified_ms, artist, title) VALUES
            (7, '/Music/Tracks/Okuma Drift - Neon Rain.mp3', '/Music/Tracks/Okuma Drift - Neon Rain.mp3', 9, 1700,
             'Okuma Drift', 'Neon Rain');
        INSERT INTO get_lines (id, position, raw, dedupe_key, added_at) VALUES (1, 0, 'Okuma Drift - Neon Rain', 'k', 1);
        INSERT INTO inbox_items (id, kind, relative_path, state, created_at) VALUES
            (1, 'single', 'Singles/Okuma Drift - Neon Rain.mp3', 'ready', 1);
        INSERT INTO history (id, completed_at, kind, peer, remote_path, size_bytes) VALUES
            (1, 1, 'single', 'vinyl_ghost', X'61', 5);
    )sql"_s))
        << db.lastError().toStdString();

    const auto result = selecta::storage::migrate(db, all);
    ASSERT_TRUE(result.ok()) << result.error.toStdString();

    EXPECT_EQ(rows(u"SELECT id, modified_ms, genre, title FROM collection_files;"_s),
              QStringList{u"7|-1||Neon Rain"_s});
    EXPECT_EQ(rows(u"SELECT upgrade_of FROM get_lines;"_s), QStringList{QString()});
    EXPECT_EQ(rows(u"SELECT replaces FROM inbox_items;"_s), QStringList{QString()});
    EXPECT_EQ(rows(u"SELECT likely_source_kbps FROM history;"_s), QStringList{u"null"_s});
    // A verdict is a bitrate.
    EXPECT_FALSE(db.exec(u"UPDATE history SET likely_source_kbps = 0;"_s));
    EXPECT_TRUE(db.exec(u"UPDATE history SET likely_source_kbps = 128;"_s));
}
