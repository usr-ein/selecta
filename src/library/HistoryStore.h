// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::library {

// One completed download (PRD DL-9): what came from whom, and where the file is now. Kept forever, so Selecta can
// say "downloaded before" and the History view can list each session's downloads.
struct HistoryEntry
{
    enum class Kind { Single, Album, Preview };
    // Where the file went after arriving: still in the Inbox, filed into the collection, trashed, or a preview that
    // stayed in the preview cache.
    enum class Whereabouts { Inbox, Filed, Trashed, Preview };

    qint64 id = 0;
    std::optional<qint64> session;
    qint64 completedAt = 0; // Unix time, ms
    Kind kind = Kind::Single;
    QString peer;
    QByteArray remotePath; // the peer's path, as sent (raw bytes: not always UTF-8)
    qint64 sizeBytes = 0;
    QString quality; // "FLAC 16/44.1", "MP3 320"…
    QString artist;
    QString title;
    QString version;
    std::optional<qint64> inboxItem; // cleared when the Inbox item goes
    Whereabouts whereabouts = Whereabouts::Inbox;
    QString filedPath; // where it was filed, for Whereabouts::Filed
};

// Reads and writes the download history. Uses the Database on its owning thread; every method reports failure through
// its return value and the database's lastError().
class HistoryStore
{
public:
    explicit HistoryStore(storage::Database &db)
        : m_db(db)
    {}

    std::optional<qint64> add(const HistoryEntry &entry);
    // Records where an entry's file went. `filedPath` is kept only for Whereabouts::Filed.
    [[nodiscard]] bool setWhereabouts(qint64 id, HistoryEntry::Whereabouts whereabouts, const QString &filedPath = {});
    // The same for every entry that arrived as this Inbox item (an album has one entry per file). Returns how many
    // changed, or -1 on failure.
    int setWhereaboutsForInboxItem(qint64 inboxItem, HistoryEntry::Whereabouts whereabouts,
                                   const QString &filedPath = {});
    // The entries that arrived as this Inbox item.
    QList<qint64> idsForInboxItem(qint64 inboxItem);
    // These entries are in the Inbox again, as `inboxItem` (an Undo of a move to the Trash). Returns how many changed,
    // or -1 on failure.
    int returnToInbox(const QList<qint64> &ids, qint64 inboxItem);

    std::optional<HistoryEntry> find(qint64 id);
    // A session's downloads, oldest first.
    QList<HistoryEntry> forSession(qint64 session);
    // The newest `limit` downloads, newest first.
    QList<HistoryEntry> recent(int limit);
    // Every download except previews, oldest first (the matcher's "downloaded before" input).
    QList<HistoryEntry> downloads();
    // Whether this exact file was downloaded from this peer before.
    bool contains(const QString &peer, const QByteArray &remotePath);

private:
    storage::Database &m_db;
};

} // namespace selecta::library
