// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/FolderRoles.h"
#include "collection/Scanner.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "match/MatchIndex.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <variant>

class QTimer;

namespace selecta::storage {
class Database;
}
namespace selecta::collection {
class Watcher;
}

namespace selecta::core {

// What the Collection view shows.
struct CollectionStatus
{
    QStringList roots; // the tracks folder and the albums folder (those chosen)
    bool scanning = false;
    int done = 0;  // files read so far in this scan
    int total = 0; // files this scan reads (new or changed ones)
    int tracks = 0;
    int albums = 0;                     // the albums folder's subfolders that hold audio (collection::albumsOf)
    qint64 bytes = 0;                   // the files' total size
    QList<QPair<QString, int>> formats; // "MP3 320", "FLAC"… with how many tracks, the most first (LIB-5)
    int unreadable = 0;
    QList<QPair<QString, QString>> unreadableSample; // (path, reason), at most kUnreadableSample
    qint64 lastScanMs = 0;                           // Unix time, ms; 0 before the first
    QString message;                                 // a problem worth saying, in words
};

// The collection index (PRD LIB-1…LIB-4, DL-11) on the engine thread: the loose tracks of the tracks folder and the
// albums of the albums folder (collection::FolderRoles), kept in the database (collection_files), from scans that read
// only new or changed files (collection::scan), kept current by watching the folders (collection::Watcher). From it,
// the Inbox and the download history, it builds the match::MatchIndex snapshot that Get tracks lines and search
// results are checked against ("you have it"), with one album per album folder (collection::albumsOf).
//
// Read-only on the user's files, always. Scans and index builds run one at a time on a worker; their results come back
// through a mutex-guarded mailbox. A folder that isn't there (an unplugged drive) is reported, and the files known
// under it are kept.
//
// Threading: engine thread. Ownership: the database outlives it; the destructor cancels a scan and waits for it.
class CollectionService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;
    static constexpr int kUnreadableSample = 20;

    struct Options
    {
        int readers = 4;          // files read at once during a scan
        bool watch = true;        // follow changes with collection::Watcher (tests turn it off)
        int rebuildDelayMs = 500; // coalesces Inbox changes before the index is rebuilt
    };

    CollectionService(storage::Database &db, Options options, WallClock now = {}, QObject *parent = nullptr);
    ~CollectionService() override;
    CollectionService(const CollectionService &) = delete;
    CollectionService &operator=(const CollectionService &) = delete;
    CollectionService(CollectionService &&) = delete;
    CollectionService &operator=(CollectionService &&) = delete;

    // The collection folders with their roles, and folders under them that aren't part of the collection (Selecta's
    // own). Files outside the folders are forgotten; when the folders or their roles changed (or at the first call) a
    // full scan follows, which forgets the files the roles leave out, and the index is rebuilt.
    void setFolders(const collection::FolderRoles &folders, const QStringList &excluded);
    // Scans every root again (new and changed files are read).
    void scanNow();
    // What the index knows besides the collection: Inbox items and past downloads (from their own stores).
    void setInbox(const QList<library::InboxItem> &items);
    void setHistory(const QList<library::HistoryEntry> &entries);

    // The latest snapshot (empty until the first build).
    std::shared_ptr<const match::MatchIndex> index() const { return m_index; }
    CollectionStatus status() const { return m_status; }
    // Blocks until scans and index builds in flight are done and recorded. For tests.
    void waitForIdle();

Q_SIGNALS:
    void statusChanged(const selecta::core::CollectionStatus &status);
    void indexChanged(const std::shared_ptr<const selecta::match::MatchIndex> &index);

private Q_SLOTS:
    void drain();

private:
    struct ScanOutcome
    {
        collection::ScanResult result;
        QStringList folders;
        bool full = false;
    };
    struct IndexOutcome
    {
        std::shared_ptr<const match::MatchIndex> index;
        int albums = 0;
    };
    struct ProgressOutcome
    {
        int done = 0;
        int total = 0;
    };
    using Outcome = std::variant<ScanOutcome, IndexOutcome, ProgressOutcome>;

    void startScan(const QStringList &folders, bool full);
    void scanFinished(const ScanOutcome &outcome);
    void scheduleRebuild();
    void rebuild();
    void post(Outcome outcome);
    void publish();

    storage::Database &m_db;
    Options m_options;
    WallClock m_now;
    collection::FolderRoles m_folders;
    QStringList m_roots; // m_folders.roots()
    QStringList m_excluded;
    bool m_rootsSet = false;
    QList<library::InboxItem> m_inboxItems;
    QList<library::HistoryEntry> m_history;
    std::shared_ptr<const match::MatchIndex> m_index;
    quint64 m_generation = 0;
    CollectionStatus m_status;
    collection::Watcher *m_watcher = nullptr;
    QTimer *m_rebuildTimer = nullptr;
    // Work on the worker, one job at a time.
    QThreadPool m_pool;
    std::shared_ptr<audio::CancellationFlag> m_cancel; // the scan in flight
    bool m_scanning = false;
    bool m_building = false;
    bool m_rebuildWanted = false;
    bool m_fullScanWanted = false;
    QStringList m_foldersWanted; // changed folders that wait for the scan in flight
    std::mutex m_mutex;
    QList<Outcome> m_outcomes; // guarded by m_mutex
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::CollectionStatus)
