// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/FolderRoles.h"
#include "core/InboxRetag.h"
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
#include <QStringView>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// Where the library lives and how files are named (Settings → Library and File names). Paths are absolute.
struct LibraryConfig
{
    QString inboxRoot;  // Selecta's own Inbox, in its data folder: never the user's choice
    QString tracksRoot; // the tracks folder, of loose tracks: filed singles go there; empty until chosen
    QString albumsRoot; // the albums folder, one album per subfolder: filed albums go there; empty until chosen
    library::naming::Templates templates;
    library::naming::Cleaner cleaner;

    // The collection's folders, with their roles.
    collection::FolderRoles folderRoles() const { return {tracksRoot, albumsRoot}; }
};

// Selecta's layout inside the Inbox (PRD DL-3): singles in one flat folder, albums in a folder each.
inline constexpr QStringView kInboxSinglesFolder = u"Singles";
inline constexpr QStringView kInboxAlbumsFolder = u"Albums";

// An upgrade (InboxItem::replaces, P-010): why filing it can't put it in its collection file's place, in words, from the
// names alone; empty when it can, as far as they tell (filing checks the rest). The name stays exactly the same, so
// the two must be files of the same type (PRD D41). Pure.
QString upgradeMismatch(const library::InboxItem &item);

// Why a library configuration can't be used, or an empty string when it can. The Inbox must be an absolute path. The
// tracks and albums folders may be unset; a folder that is set must be an absolute path, and the two must stay apart
// from each other and from the Inbox: not the same folder, neither inside the other.
QString checkLibraryConfig(const LibraryConfig &config);

// One Inbox row as the UI sees it.
struct InboxEntry
{
    library::InboxItem item;
    bool busy = false;   // being filed or trashed right now
    QStringList alsoOn;  // a single: the albums it is also on (DL-8)
    QList<qint64> flags; // …and the supersede flags saying so (EngineHost::notTheSameTrack())
};

// An Inbox item Selecta moved to the Trash, with what it takes to put it back (Undo).
struct TrashedItem
{
    library::InboxItem item;
    QString trashedPath;   // where it is in the Trash
    QList<qint64> history; // its history entries
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
    // Records an item the arrival pipeline just published into the Inbox, and publishes the list. Returns its id.
    std::optional<qint64> add(const library::InboxItem &item);
    // Files these items into the collection: singles into the tracks folder, albums into the albums folder, keeping
    // the names they got on arrival. Busy, unknown or still-processing items are skipped; an item whose folder isn't
    // chosen yet stays, and finished() says so. An upgrade (InboxItem::replaces, P-010) takes its collection file's
    // place instead, under its exact path, and that file goes to the Trash (FileOps::replaceInCollection()); one that
    // can't (another type of file, a folder no longer there or no longer in the collection) is filed as a new file,
    // and finished() says why.
    void file(const QList<qint64> &ids);
    // Moves these items to the macOS Trash. trashed() reports each one that went.
    void trash(const QList<qint64> &ids);
    // Puts an item trashed() reported back into the Inbox (Undo), under its old name when it's free. finished()
    // reports the outcome, with the item's new id.
    void restore(const selecta::core::TrashedItem &trashed);
    // New tags for a single (DL-4, META-4): `changes` written over its own tags (a front cover in them replaces the
    // embedded one), `cleared` fields removed, then the file is renamed after its new artist and title. `tagSource`
    // and `releaseJson`, when given, say where the tags came from now. finished() reports the outcome.
    void retag(qint64 id, const convert::MetadataModel &changes, const QStringList &cleared,
               const QString &tagSource = {}, const QString &releaseJson = {});

    QList<InboxEntry> entries();
    // Publishes the list again (changed(): flags on the singles changed, for instance).
    void refresh() { publish(); }
    std::optional<library::InboxItem> item(qint64 id) { return m_inbox.find(id); }
    // Where an item is on disk.
    QString absolutePath(const library::InboxItem &item) const;
    // Blocks until file work in flight is done and its results are recorded. For shutdown and tests.
    void waitForIdle();

Q_SIGNALS:
    void changed(const QList<selecta::core::InboxEntry> &entries);
    // One item's filing or trashing ended. `message` says where it went, or why it didn't.
    void finished(qint64 id, bool ok, const QString &message);
    // An item went to the Trash.
    void trashed(const selecta::core::TrashedItem &trashed);

private:
    enum class Work { File, Trash, Retag, Restore, Replace };
    struct Outcome
    {
        qint64 id = 0;
        Work work = Work::File;
        library::FileResult result;
        RetagResult retag; // Work::Retag
        QString tagSource;
        QString releaseJson;
        TrashedItem restored; // Work::Restore
        QString note;         // Work::File: why an upgrade couldn't take its collection file's place
    };

    void start(qint64 id, Work work, std::function<library::FileResult()> job);
    // Files an upgrade in its collection file's place, or as a new file when it can't be (Outcome::note says why).
    void startUpgrade(qint64 id, const QString &source, const QString &replaces, const QString &tracksRoot,
                      const QString &name);
    void post(Outcome outcome);
    void drain();
    void retagged(const Outcome &outcome);
    void restored(const Outcome &outcome);
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
Q_DECLARE_METATYPE(selecta::core::TrashedItem)
