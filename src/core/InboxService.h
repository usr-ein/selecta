// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/FileOps.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "library/Naming.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// Where the library lives and how files are named (Settings → Library and Filing). Paths are absolute.
struct LibraryConfig
{
    QString inboxRoot;
    QStringList collectionRoots; // filing goes into the first one
    QString singlesFolder = QStringLiteral("Singles");
    QString albumsFolder = QStringLiteral("Albums");
    library::naming::Templates templates;
    library::naming::Cleaner cleaner;
};

// Why a library configuration can't be used, or an empty string when it can: the Inbox and every collection folder
// must be absolute, distinct and not inside one another (filing would otherwise move files within one tree).
QString checkLibraryConfig(const LibraryConfig &config);

// One Inbox row as the UI sees it.
struct InboxEntry
{
    library::InboxItem item;
    bool busy = false; // being filed or trashed right now
};

// The Inbox (PRD DL-3 to DL-9) on the engine thread: lists what's waiting, files items into the collection and moves
// them to the Trash. File work runs on a small pool of its own (copies across volumes can take minutes); each result
// comes back to this thread, which then updates the Inbox and the history. Items Selecta didn't put in the Inbox are
// never touched.
//
// Threading: engine thread. Ownership: owns its stores' use of the engine's Database; the destructor waits for file
// work in flight and records its results, so a quit never loses track of a filed item.
class InboxService : public QObject
{
    Q_OBJECT

public:
    InboxService(storage::Database &db, LibraryConfig config, library::FileOps::TrashFunction trash = {},
                 QObject *parent = nullptr);
    ~InboxService() override;
    InboxService(const InboxService &) = delete;
    InboxService &operator=(const InboxService &) = delete;
    InboxService(InboxService &&) = delete;
    InboxService &operator=(InboxService &&) = delete;

    void setConfig(const LibraryConfig &config);
    const LibraryConfig &config() const { return m_config; }

    // Forgets items whose files were deleted outside Selecta (recording them as trashed), then publishes the list.
    void reconcile();
    // Files these items into the first collection folder: singles into the Singles folder, albums into the Albums
    // folder, keeping the names they got on arrival. Busy, unknown or still-processing items are skipped.
    void file(const QList<qint64> &ids);
    // Moves these items to the macOS Trash.
    void trash(const QList<qint64> &ids);

    QList<InboxEntry> entries();
    // Where an item is on disk.
    QString absolutePath(const library::InboxItem &item) const;
    // Blocks until file work in flight is done and its results are recorded. For shutdown and tests.
    void waitForIdle();

Q_SIGNALS:
    void changed(const QList<selecta::core::InboxEntry> &entries);
    // One item's filing or trashing ended. `message` says where it went, or why it didn't.
    void finished(qint64 id, bool ok, const QString &message);

private:
    enum class Work { File, Trash };
    struct Outcome
    {
        qint64 id = 0;
        Work work = Work::File;
        library::FileResult result;
    };

    void start(qint64 id, Work work, std::function<library::FileResult()> job);
    void drain();
    void publish();
    void rebuildFileOps();

    storage::Database &m_db;
    LibraryConfig m_config;
    library::FileOps::TrashFunction m_trash;
    library::InboxStore m_inbox;
    library::HistoryStore m_history;
    std::shared_ptr<const library::FileOps> m_fileOps;
    QSet<qint64> m_busy;
    QThreadPool m_pool;
    std::mutex m_outcomesMutex;
    QList<Outcome> m_outcomes; // guarded by m_outcomesMutex: filled by workers, drained on the engine thread
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::InboxEntry)
