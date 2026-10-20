// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

namespace selecta::core {

// The preview cache (PRD PREV-8, CACHE-1): the audio of previews from peers (TransferService) and from YouTube
// (YouTubePreviews), kept on disk so that a preview heard before plays again with no second transfer and no yt-dlp
// run, after a restart too. Preview audio is never filed, shared, converted or tagged (D20, D26): only Keep copies a
// peer's preview into the Inbox, and the folder is kept out of the collection and the shares.
//
// - An index (index.json in the folder) lists each complete file under a key, with its size and when it was last
//   used. It is rewritten atomically after every change.
// - Bounded: the complete files total at most Options::maxBytes (2 GB), the least recently used going first. A file
//   unused for kExpiry (30 days) goes as well.
// - Opening the folder deletes whatever the index doesn't vouch for: files that were being written when Selecta quit
//   or crashed, files whose size changed, files from an earlier layout. An unreadable index starts the cache empty.
// - Pinned files (being written, or playing) are never evicted, expired, cleared or deleted as strays. remove()
//   (Discard, PREV-5) deletes even a pinned file: a player reading it keeps its open handle.
// - Notes: short values kept alongside the files and expiring like them (the YouTube video picked for a release).
// - Nothing outside the folder is ever read or deleted, whatever the index says (symbolic links are not followed).
//
// Threading: one thread (the engine's); not thread-safe. Ownership: the Engine owns it and destroys it after the
// services that use it. Failure modes: a folder that can't be created or written makes the cache unusable
// (isUsable() false: reserve() fails, find() misses, commit() refuses); a failed index write is logged, and the next
// change writes the index again (at worst, the next launch deletes files the stale index doesn't list).
class PreviewCache
{
public:
    using WallClock = std::function<qint64()>; // Unix time, ms

    static constexpr qint64 kDefaultMaxBytes = qint64{2} * 1000 * 1000 * 1000;
    static constexpr std::chrono::days kExpiry{30};
    static constexpr qsizetype kMaxNotes = 10'000;
    static constexpr qsizetype kMaxTextChars = 2048; // a key, a note or a remembered value
    static constexpr qint64 kMaxIndexBytes =
        qint64{8} * 1024 * 1024; // a bigger index is damaged: the cache starts empty

    struct Options
    {
        qint64 maxBytes = kDefaultMaxBytes;
        WallClock now; // default: the system clock
    };

    // A complete file in the cache.
    struct File
    {
        QString path; // absolute
        qint64 bytes = 0;
        QString note; // what was committed with it (for YouTube audio: the channel)
    };

    // Opens (and creates) the folder, then deletes what the index doesn't vouch for, expired files and the least
    // recently used beyond the bound. An empty `directory` makes an unusable cache.
    PreviewCache(const QString &directory, Options options);
    explicit PreviewCache(const QString &directory); // the default bound, the system clock
    ~PreviewCache();
    PreviewCache(const PreviewCache &) = delete;
    PreviewCache &operator=(const PreviewCache &) = delete;
    PreviewCache(PreviewCache &&) = delete;
    PreviewCache &operator=(PreviewCache &&) = delete;

    // The folder, symbolic links resolved; empty when unusable.
    QString directory() const { return m_directory; }
    bool isUsable() const { return m_usable; }

    // The complete file kept under `key`, now the most recently used. nullopt when there is none, or when its file is
    // gone, changed size or expired (the entry is then forgotten and a changed file deleted).
    std::optional<File> find(const QString &key);
    // A new, empty file in the folder for a preview to be written into, named after `name`. It is pinned until it is
    // abandoned, or committed and then unpinned by its owner. Empty when the folder can't take it.
    QString reserve(const QString &name);
    // `path`, a complete file in the folder, is kept under `key` from now on, with `note`. The key's earlier file is
    // deleted, and a file belongs to one key only. Then the expired and the least recently used files beyond the bound
    // go. False, keeping nothing, for an empty file or a path outside the folder.
    bool commit(const QString &key, const QString &path, const QString &note = {});
    // Lets go of a file: one pin() (or the reserve()) of it ends, and then a file the cache doesn't keep under a key
    // is deleted, unless something else still pins it.
    void abandon(const QString &path);
    // Deletes the file kept under `key`, pinned or not (Discard). False when there was none.
    bool remove(const QString &key);

    // Files in use: a path, or a prefix such as "<folder>/YouTube/<video id>." for the files a tool writes under names
    // it chooses. Counted: each pin() needs one unpin().
    void pin(const QString &pathOrPrefix);
    void unpin(const QString &pathOrPrefix);

    // Notes: a short value remembered under a key (at most kMaxNotes, the least recently used going first).
    std::optional<QString> recall(const QString &key);
    void remember(const QString &key, const QString &value);
    void forget(const QString &key);

    // What the folder holds in bytes, files being written included and the index excluded (CACHE-2).
    qint64 totalBytes() const;
    // Empties the cache (CACHE-2): every file but the pinned ones, and the notes.
    void clear();

private:
    struct Entry
    {
        QString file; // relative to the folder
        qint64 bytes = 0;
        qint64 usedAt = 0;
        QString note;
    };
    struct Note
    {
        QString value;
        qint64 usedAt = 0;
    };

    void load();
    bool save();
    // Deletes the files the index doesn't list, except pinned ones and the index itself.
    void removeStrays();
    void expire();
    void trim();
    bool isPinned(const QString &path) const;
    bool isExpired(qint64 usedAt) const;
    // The absolute path of a name from the index; empty for a name that isn't a plain relative path.
    QString pathOf(const QString &relative) const;
    // The name of `path` in the folder: a regular file inside it, symbolic links resolved; nullopt otherwise.
    std::optional<QString> relativeOf(const QString &path) const;
    void deleteFile(const QString &relative) const;
    QString indexPath() const;

    QString m_directory;
    Options m_options;
    bool m_usable = false;
    QHash<QString, Entry> m_entries; // by key
    QHash<QString, Note> m_notes;    // by key
    QHash<QString, int> m_pins;      // path or prefix → how many hold it
};

} // namespace selecta::core
