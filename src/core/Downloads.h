// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GetTracks.h"
#include "meta/Release.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// One file Selecta wants from a peer (PRD DL-1, DL-2), from the moment it is asked for until it has arrived in the
// Inbox, failed for good or been cancelled. Kept in the database, so the queue survives a restart.
struct Download
{
    enum class State {
        Waiting,      // not asked for yet: offline, paced by the governor, or waiting for a retry (retryAtMs)
        Queued,       // asked for; waiting in the peer's queue
        Transferring, // the file is arriving
        Converting,   // complete; being converted, tagged and published into the Inbox
        Arrived,      // in the Inbox (inboxItem)
        Failed,       // for good: message says why
        Cancelled,
    };

    qint64 id = 0;
    QString peer;
    QByteArray remotePath; // as the peer sent it
    qint64 size = 0;
    std::optional<FileChoice> file;       // the facts the file was chosen on: quality, length
    std::optional<meta::Release> release; // what to tag it from
    std::optional<qint64> getLine;        // the Get tracks line it is for
    std::optional<qint64> album;          // the album download it is part of (its files arrive together)
    State state = State::Waiting;
    QString message; // what is happening or what went wrong, in words
    int attempts = 0;
    std::optional<qint64> retryAtMs; // Unix time, ms
    qint64 received = 0;             // bytes on disk
    QString localPath;               // the original, while it downloads and converts
    std::optional<qint64> inboxItem;
    qint64 createdAt = 0; // Unix time, ms
    qint64 updatedAt = 0;

    bool isFinished() const { return state == State::Arrived || state == State::Failed || state == State::Cancelled; }
    // The file name at the end of the remote path ("C:\Music\Artist - Title.flac" → "Artist - Title.flac").
    QString remoteFileName() const;
};

// "waiting", "queued"… as stored.
QString downloadStateName(Download::State state);

// Reads and writes downloads (table downloads). Uses the Database on its owning thread; failures are reported through
// return values and the database's lastError().
class DownloadStore
{
public:
    explicit DownloadStore(storage::Database &db)
        : m_db(db)
    {}

    // Adds a download and returns its id. Fails (nullopt) when the same file from the same peer is already being
    // downloaded.
    std::optional<qint64> add(const Download &download);
    [[nodiscard]] bool update(const Download &download);
    std::optional<Download> find(qint64 id);
    // The download of this file that hasn't finished yet, if any.
    std::optional<Download> findUnfinished(const QString &peer, const QByteArray &remotePath);
    // Every download that hasn't finished, oldest first.
    QList<Download> unfinished();
    // A Get tracks line's downloads, oldest first.
    QList<Download> forLine(qint64 lineId);
    // An album download's files, oldest first.
    QList<Download> forAlbum(qint64 albumId);
    // Forgets finished downloads last changed before `beforeMs` (they live on in the history). Returns how many went,
    // or -1 on failure.
    int pruneFinished(qint64 beforeMs);

private:
    storage::Database &m_db;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::Download)
