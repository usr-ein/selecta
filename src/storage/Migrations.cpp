// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/Migrations.h"

#include "storage/Database.h"

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::storage {
namespace {

constexpr std::array kMigrations{
    Migration{1, R"sql(
        -- One row per run of Selecta, from launch to quit (PRD D8). The id is the session number shown to the user.
        CREATE TABLE sessions (
            id         INTEGER PRIMARY KEY,
            started_at INTEGER NOT NULL, -- Unix time, ms
            ended_at   INTEGER           -- NULL while running, or when Selecta didn't quit cleanly
        ) STRICT;
    )sql"},
};

bool isValidList(std::span<const Migration> migrations)
{
    int previous = 0;
    for (const Migration &migration : migrations) {
        if (migration.version != previous + 1 || migration.sql == nullptr) {
            return false;
        }
        previous = migration.version;
    }
    return true;
}

} // namespace

MigrationResult migrate(Database &db, std::span<const Migration> migrations)
{
    if (!isValidList(migrations)) {
        return {MigrationStatus::InvalidList, db.userVersion(), u"migration versions must be 1, 2, 3, …"_s};
    }

    int current = db.userVersion();
    if (current < 0) {
        return {MigrationStatus::Failed, current, db.lastError()};
    }

    const int latest = migrations.empty() ? 0 : migrations.back().version;
    if (current > latest) {
        return {MigrationStatus::DatabaseTooNew, current,
                u"the database is at version %1, but this Selecta only knows up to %2"_s.arg(current).arg(latest)};
    }

    for (const Migration &migration : migrations) {
        if (migration.version <= current) {
            continue;
        }
        Transaction transaction(db);
        if (!transaction.isActive() || !db.exec(QString::fromUtf8(migration.sql)) ||
            !db.setUserVersion(migration.version) || !transaction.commit()) {
            const QString error = u"migration %1 failed: "_s.arg(migration.version) + db.lastError();
            qCWarning(lcStorage) << error;
            return {MigrationStatus::Failed, current, error};
        }
        qCInfo(lcStorage) << "applied migration" << migration.version;
        current = migration.version;
    }
    return {MigrationStatus::Ok, current, {}};
}

std::span<const Migration> selectaMigrations()
{
    return kMigrations;
}

} // namespace selecta::storage
