// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/MatchCorrections.h"
#include "meta/Release.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

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

// A single that is also on an album (PRD DL-8), flagged for the user: the pair is shown in the Inbox and Collection
// views. A single in the Inbox can be moved to the Trash from there; one filed in the collection is never touched.
struct SupersedeFlag
{
    qint64 id = 0;
    QString album;                    // the album's title
    std::optional<qint64> albumItem;  // the album in the Inbox, while it's there
    std::optional<qint64> singleItem; // an Inbox single…
    QString singlePath;               // …or a single filed in the collection
    QString single;                   // "Artist – Title"
    QString summary;                  // the decision and why
    qint64 createdAt = 0;             // Unix time, ms
    // The pair as corrections know it (LIB-6), for "Not the Same Track"; invalid on flags raised before.
    TrackIdentity singleTrack;
    TrackIdentity albumTrack;
};

// Reads and writes supersede flags (table supersede_flags). A flag on an Inbox single goes with it. Uses the Database
// on its owning thread; failures are reported through return values and the database's lastError().
class SupersedeFlagStore
{
public:
    explicit SupersedeFlagStore(storage::Database &db)
        : m_db(db)
    {}

    // Records a flag, unless the same single is already flagged for the same album. Returns its id.
    std::optional<qint64> add(const SupersedeFlag &flag);
    // Every flag, newest first.
    QList<SupersedeFlag> all();
    std::optional<SupersedeFlag> find(qint64 id);
    // The flags on each Inbox single, by Inbox item.
    QHash<qint64, QList<qint64>> idsByInboxItem();
    // The albums each flagged Inbox single is also on, by Inbox item.
    QHash<qint64, QStringList> albumsByInboxItem();
    [[nodiscard]] bool remove(qint64 id);

private:
    storage::Database &m_db;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::AlbumDownload)
Q_DECLARE_METATYPE(selecta::core::SupersedeFlag)
