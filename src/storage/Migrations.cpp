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
    Migration{3, R"sql(
        -- Get tracks (PRD BATCH-1…9, D27): the list the user pasted, one row per line, kept across launches.
        CREATE TABLE get_lines (
            id                 INTEGER PRIMARY KEY,
            position           INTEGER NOT NULL,          -- order in the list
            raw                TEXT NOT NULL,             -- the line as pasted
            dedupe_key         TEXT NOT NULL UNIQUE,      -- normalised artist + title: the same track twice is one line
            artist             TEXT NOT NULL DEFAULT '',
            title              TEXT NOT NULL DEFAULT '',  -- with its version text
            duration_hint_ms   INTEGER,                   -- a length written in the line or given by a playlist
            isrc               TEXT NOT NULL DEFAULT '',
            label_hint         TEXT NOT NULL DEFAULT '',
            release_state      TEXT NOT NULL DEFAULT 'pending'
                               CHECK (release_state IN ('pending', 'resolved', 'ambiguous', 'none', 'failed')),
            release_json       TEXT,                      -- the chosen release (meta::Release)
            release_confidence TEXT NOT NULL DEFAULT '',
            find_state         TEXT NOT NULL DEFAULT 'idle'
                               CHECK (find_state IN ('idle', 'queued', 'searching', 'ready', 'check', 'notfound',
                                                     'owned', 'inbox')),
            best_json          TEXT,                      -- the chosen file on Soulseek
            ticked             INTEGER NOT NULL DEFAULT 0 CHECK (ticked IN (0, 1)),
            download_state     TEXT NOT NULL DEFAULT 'none'
                               CHECK (download_state IN ('none', 'queued', 'transferring', 'converting', 'arrived',
                                                         'failed')),
            message            TEXT NOT NULL DEFAULT '',  -- why a step failed, in words
            added_at           INTEGER NOT NULL           -- Unix time, ms
        ) STRICT;
        CREATE INDEX get_lines_by_position ON get_lines(position);
    )sql"},
    Migration{4, R"sql(
        -- Downloads (PRD DL-1, DL-2, D27): the files Selecta wants from peers, kept across launches until they have
        -- arrived in the Inbox, failed for good or been cancelled.
        CREATE TABLE downloads (
            id           INTEGER PRIMARY KEY,
            peer         TEXT NOT NULL,
            remote_path  BLOB NOT NULL,                -- raw bytes, as the peer sent them
            size         INTEGER NOT NULL DEFAULT 0 CHECK (size >= 0),
            file_json    TEXT,                         -- the chosen file's facts (core::FileChoice)
            release_json TEXT,                         -- the release to tag it from (meta::Release)
            get_line     INTEGER REFERENCES get_lines(id) ON DELETE SET NULL,
            state        TEXT NOT NULL CHECK (state IN ('waiting', 'queued', 'transferring', 'converting',
                                                        'arrived', 'failed', 'cancelled')),
            message      TEXT NOT NULL DEFAULT '',     -- what is happening or what went wrong, in words
            attempts     INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
            retry_at     INTEGER,                      -- Unix time, ms: when a waiting download asks again
            received     INTEGER NOT NULL DEFAULT 0 CHECK (received >= 0),
            local_path   TEXT NOT NULL DEFAULT '',     -- the original, while it downloads and converts
            inbox_item   INTEGER REFERENCES inbox_items(id) ON DELETE SET NULL,
            created_at   INTEGER NOT NULL,             -- Unix time, ms
            updated_at   INTEGER NOT NULL
        ) STRICT;
        -- One download of a file at a time: another can start once the first has finished.
        CREATE UNIQUE INDEX downloads_unfinished_file ON downloads(peer, remote_path)
            WHERE state NOT IN ('arrived', 'failed', 'cancelled');
        CREATE INDEX downloads_by_line ON downloads(get_line);
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
