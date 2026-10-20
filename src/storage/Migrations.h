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

// The migration that forgets the old Inbox's items, when the Inbox moved into Selecta's data folder (P-001, PRD D39):
// a database below this version recorded the items of another Inbox folder.
inline constexpr int kInboxMovedVersion = 18;

} // namespace selecta::storage
