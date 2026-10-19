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
    Migration{2, R"sql(
        -- The Inbox (PRD DL-3, DL-4): downloads converted and tagged on arrival (D29), waiting to be filed.
        CREATE TABLE inbox_items (
            id            INTEGER PRIMARY KEY,
            kind          TEXT NOT NULL CHECK (kind IN ('single', 'album')),
            relative_path TEXT NOT NULL UNIQUE, -- the file (single) or folder (album), relative to the Inbox root
            state         TEXT NOT NULL CHECK (state IN ('processing', 'ready', 'failed')),
            error         TEXT,
            artist        TEXT NOT NULL DEFAULT '',
            title         TEXT NOT NULL DEFAULT '', -- the track title, or the album title for albums
            version       TEXT NOT NULL DEFAULT '',
            album         TEXT NOT NULL DEFAULT '',
            year          INTEGER,
            track_count   INTEGER,
            duration_ms   INTEGER NOT NULL DEFAULT 0,
            format        TEXT NOT NULL DEFAULT '', -- as it is now, e.g. 'MP3'
            bitrate       INTEGER,                  -- kbps, lossy formats
            sample_rate   INTEGER,
            bit_depth     INTEGER,
            size_bytes    INTEGER NOT NULL DEFAULT 0 CHECK (size_bytes >= 0),
            source_format TEXT NOT NULL DEFAULT '', -- as downloaded, e.g. 'FLAC 16/44.1'
            tag_source    TEXT NOT NULL DEFAULT '', -- 'spotify', 'musicbrainz', 'deezer' or 'file'
            release_json  TEXT,                     -- the matched release used for the tags
            peer          TEXT NOT NULL DEFAULT '',
            remote_path   BLOB,                     -- raw bytes, as the peer sent them
            session       INTEGER REFERENCES sessions(id),
            created_at    INTEGER NOT NULL          -- Unix time, ms
        ) STRICT;

        -- Every completed download (PRD DL-9) and where it is now.
        CREATE TABLE history (
            id           INTEGER PRIMARY KEY,
            session      INTEGER REFERENCES sessions(id),
            completed_at INTEGER NOT NULL,
            kind         TEXT NOT NULL CHECK (kind IN ('single', 'album', 'preview')),
            peer         TEXT NOT NULL,
            remote_path  BLOB NOT NULL,
            size_bytes   INTEGER NOT NULL CHECK (size_bytes >= 0),
            quality      TEXT NOT NULL DEFAULT '',
            artist       TEXT NOT NULL DEFAULT '',
            title        TEXT NOT NULL DEFAULT '',
            version      TEXT NOT NULL DEFAULT '',
            inbox_item   INTEGER REFERENCES inbox_items(id) ON DELETE SET NULL,
            whereabouts  TEXT NOT NULL DEFAULT 'inbox' CHECK (whereabouts IN ('inbox', 'filed', 'trashed', 'preview')),
            filed_path   TEXT
        ) STRICT;
        CREATE INDEX history_by_session ON history(session, completed_at);

        -- What Selecta took from each peer per local day, so the fairness caps survive restarts (FAIR-2).
        CREATE TABLE peer_days (
            peer  TEXT NOT NULL,
            day   TEXT NOT NULL, -- ISO date
            files INTEGER NOT NULL DEFAULT 0 CHECK (files >= 0),
            bytes INTEGER NOT NULL DEFAULT 0 CHECK (bytes >= 0),
            PRIMARY KEY (peer, day)
        ) STRICT, WITHOUT ROWID;

        -- Peers that denied us, and until when Selecta leaves them alone.
        CREATE TABLE peer_backoffs (
            peer     TEXT PRIMARY KEY,
            until_at INTEGER NOT NULL, -- Unix time, ms
            reason   TEXT NOT NULL
        ) STRICT;

        -- The network activity log (PRD SAFE-4): every governor decision and notable network event.
        CREATE TABLE activity (
            id          INTEGER PRIMARY KEY,
            at          INTEGER NOT NULL,
            operation   TEXT NOT NULL,
            subject     TEXT NOT NULL DEFAULT '',
            outcome     TEXT NOT NULL, -- 'allowed', or the refusal
            explanation TEXT NOT NULL DEFAULT ''
        ) STRICT;
        CREATE INDEX activity_by_time ON activity(at);
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
