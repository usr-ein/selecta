// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <span>

namespace selecta::storage {

class Database;

struct Migration
{
    int version;     // becomes the database's user_version once applied; strictly increasing, starting at 1
    const char *sql; // one or more statements
};

enum class MigrationStatus {
    Ok,
    Failed,         // a migration failed and was rolled back; the database is at the last good version
    DatabaseTooNew, // written by a newer Selecta: refuse to touch it rather than lose data
    InvalidList,    // versions not strictly increasing from 1 (a programming error)
};

struct MigrationResult
{
    MigrationStatus status = MigrationStatus::Failed;
    int version = -1; // the database's version afterwards
    QString error;

    bool ok() const { return status == MigrationStatus::Ok; }
};

// Applies, in order and each in its own transaction, every migration whose version is above the database's
// user_version.
[[nodiscard]] MigrationResult migrate(Database &db, std::span<const Migration> migrations);

// Selecta's schema history. Append only: never edit a migration that has shipped.
std::span<const Migration> selectaMigrations();

// The migration that empties the Inbox of earlier builds (P-001, PRD D39: the Inbox is Selecta's own, in its data
// folder): a database below this version may hold their Inbox items.
inline constexpr int kInboxMovedVersion = 19;

// The migration of upgrades and the Collection view (P-010): genres in the collection index (every file read once
// more), the collection file a Get tracks line or an Inbox item upgrades, and the spectrum check's verdict in History.
inline constexpr int kCollectionUpgradesVersion = 20;

} // namespace selecta::storage
