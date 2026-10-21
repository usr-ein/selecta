// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/FolderRoles.h"
#include "core/CollectionBrowse.h"
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

// P-010: what Save to collection does with an upgrade (InboxItem::replaces), judged now.
enum class UpgradeVerdict : quint8 {
    None,        // not an upgrade
    Replaces,    // better than its collection file, or that file is gone: it takes the file's place (D41)
    NotBetter,   // its real tier isn't higher than its file's: saved as a new file, the file stays as it is
    CantReplace, // the name can't stay (another type, PRD D41), or the file's folder left the collection: saved as new
    NotKnownYet, // the collection isn't read yet (just after launch): saving waits until it is
};
struct UpgradeJudgement
{
    UpgradeVerdict verdict = UpgradeVerdict::None;
    QString note; // the verdict in words, without the file's path (the views show it as they show paths)
};

// The tier a download was got in: the quality it arrived in (InboxItem::sourceFormat, else its format and bitrate),
// lowered by the spectrum check's verdict (a fake 320, a FLAC made from an MP3). Pure.
QualityTier downloadTier(const library::InboxItem &item);
// Judges an upgrade: its real tier (downloadTier()) against its collection file's now (`tierOf`), and their types. It
// replaces only a file of a lower tier, or one the collection hasn't any more whose place, in `folders`, is free.
// Without a lookup the collection counts as not read yet. Pure but for `tierOf`.
UpgradeJudgement judgeUpgrade(const library::InboxItem &item, const TierLookup &tierOf,
                              const collection::FolderRoles &folders);

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
    // An upgrade (P-010), judged when the list was published: what Save to collection will do with it, in words.
    UpgradeVerdict upgrade = UpgradeVerdict::None;
    QString upgradeNote;
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
    // chosen yet stays, and finished() says so. An upgrade (InboxItem::replaces, P-010) judged better than its
    // collection file now (judgeUpgrade()) takes that file's place instead, at its path (FileOps::upgradeTarget()), and
    // the file goes to the Trash (FileOps::replaceInCollection()); when it can't (its folder isn't there, an unplugged
    // drive) it stays, and finished() says why. An upgrade not better, or that can't replace, is filed as a new file:
    // its file stays. One judged before the collection is read stays too.
    void file(const QList<qint64> &ids);
    // The quality tier of a collection file now: what an upgrade must beat. Publishes the list again (the verdicts).
    void setTierLookup(TierLookup lookup);
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
        // An upgrade's collection file: the one it replaced (Work::Replace), or was filed beside (Work::File).
        QString upgraded;
        bool oldThere = false; // Work::Replace: that file was there, and went to the Trash
    };

    void start(qint64 id, Work work, std::function<library::FileResult()> job);
    // Files an upgrade in its collection file's place, or, `asNew`, as a new file beside it.
    void startUpgrade(qint64 id, const QString &source, const QString &replaces, const QString &tracksRoot,
                      const QString &name, bool asNew);
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
    TierLookup m_tierOf;
    QSet<qint64> m_busy;
    QThreadPool m_pool;
    std::mutex m_outcomesMutex;
    QList<Outcome> m_outcomes; // guarded by m_outcomesMutex: filled by workers, drained on the engine thread
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::InboxEntry)
Q_DECLARE_METATYPE(selecta::core::TrashedItem)
