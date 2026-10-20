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

// One item in the Inbox (PRD DL-3, DL-4): a single file, or an album folder.
struct InboxItem
{
    enum class Kind { Single, Album };
    enum class State { Processing, Ready, Failed };

    qint64 id = 0;
    Kind kind = Kind::Single;
    QString relativePath; // relative to the Inbox root; unique
    State state = State::Processing;
    QString error; // why processing failed
    QString artist;
    QString title; // track title, or album title for albums
    QString version;
    QString album;
    std::optional<int> year;
    std::optional<int> trackCount;
    qint64 durationMs = 0;
    QString format; // as it is now, e.g. "MP3"
    std::optional<int> bitrate;
    std::optional<int> sampleRate;
    std::optional<int> bitDepth;
    qint64 sizeBytes = 0;
    QString sourceFormat; // as downloaded, e.g. "FLAC 16/44.1"
    QString tagSource;    // "spotify", "musicbrainz", "deezer" or "file"
    QString releaseJson;  // the matched release used for the tags
    QString peer;
    QByteArray remotePath;
    std::optional<int> session;
    qint64 createdAt = 0; // Unix time, ms

    friend bool operator==(const InboxItem &, const InboxItem &) = default;
};

// Reads and writes Inbox items. Uses the Database on its owning thread. Every method reports failure through its
// return value; the database's lastError() says why.
class InboxStore
{
public:
    explicit InboxStore(storage::Database &db)
        : m_db(db)
    {}

    // Adds an item and returns its id. Fails if its relative path is already in the Inbox.
    std::optional<qint64> add(const InboxItem &item);
    // Replaces every field of the item with this id.
    [[nodiscard]] bool update(const InboxItem &item);
    [[nodiscard]] bool setState(qint64 id, InboxItem::State state, const QString &error = {});
    [[nodiscard]] bool remove(qint64 id);

    std::optional<InboxItem> find(qint64 id);
    std::optional<InboxItem> findByPath(const QString &relativePath);
    // Every item, newest first.
    QList<InboxItem> all();
    int count();

private:
    storage::Database &m_db;
};

} // namespace selecta::library
