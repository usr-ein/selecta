// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// A remote folder downloaded whole (PRD DL-1, DL-3, DL-12): its files are downloads of their own (Download::album)
// and arrive together, as one Inbox album.
struct AlbumDownload
{
    enum class State {
        Listing,     // asking the peer what the folder holds
        Downloading, // its files are downloading
        Converting,  // every file is in: converting, tagging and publishing the album
        Arrived,     // in the Inbox (inboxItem)
        Partial,     // in the Inbox, without the files that couldn't be downloaded
        Failed,
        Cancelled,
    };

    qint64 id = 0;
    QString peer;
    QByteArray folder;                    // as the peer spells it
    std::optional<meta::Release> release; // a release on the album: its album gives the track list
    State state = State::Listing;
    QString message;
    std::optional<qint64> inboxItem;
    qint64 createdAt = 0; // Unix time, ms
    qint64 updatedAt = 0;

    bool isFinished() const
    {
        return state == State::Arrived || state == State::Partial || state == State::Failed ||
               state == State::Cancelled;
    }
    // The folder's own name, without the folders above it.
    QString folderName() const;
};

QString albumStateName(AlbumDownload::State state);

// Reads and writes album downloads (table album_downloads). Uses the Database on its owning thread; failures are
// reported through return values and the database's lastError().
class AlbumStore
{
public:
    explicit AlbumStore(storage::Database &db)
        : m_db(db)
    {}

    std::optional<qint64> add(const AlbumDownload &album);
    [[nodiscard]] bool update(const AlbumDownload &album);
    std::optional<AlbumDownload> find(qint64 id);
    // Every album download not finished yet, oldest first.
    QList<AlbumDownload> unfinished();

private:
    storage::Database &m_db;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::AlbumDownload)
