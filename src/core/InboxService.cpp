// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxService.h"

#include "core/Albums.h"
#include "storage/Database.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QThread>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcInbox, "selecta.core.inbox")

// Copies are disk-bound: two at a time keeps one volume busy without starving it.
constexpr int kFileWorkers = 2;

using library::FileResult;
using library::InboxItem;
using Whereabouts = library::HistoryEntry::Whereabouts;

// Whether one of the two folders is the other or inside it (compared as the scanner compares paths: NFC).
bool overlap(const QString &first, const QString &second)
{
    const QString a = collection::comparisonKey(first);
    const QString b = collection::comparisonKey(second);
    return a == b || a.startsWith(b.endsWith(u'/') ? b : b + u'/') || b.startsWith(a.endsWith(u'/') ? a : a + u'/');
}

} // namespace

QualityTier downloadTier(const library::InboxItem &item)
{
    match::AudioQuality quality = qualityOfText(item.sourceFormat);
    if (quality.codec == match::Codec::Unknown && quality.bitrateKbps == 0) { // no source quality kept: as it is now
        quality = qualityOfText(item.format);
        quality.bitrateKbps = item.bitrate.value_or(0);
    }
    return qualityTier(quality, TierEvidence{.fromLossless = false, .likelySourceKbps = item.qualitySourceKbps});
}

UpgradeJudgement judgeUpgrade(const library::InboxItem &item, const TierLookup &tierOf,
                              const collection::FolderRoles &folders)
{
    if (item.replaces.isEmpty()) {
        return {};
    }
    const auto judged = [](UpgradeVerdict verdict, QString note) {
        return UpgradeJudgement{.verdict = verdict, .note = std::move(note)};
    };
    if (item.kind != library::InboxItem::Kind::Single) {
        return judged(UpgradeVerdict::CantReplace,
                      InboxService::tr("An album can't take a track's place: Save to collection files it as an album."));
    }
    const QString target = library::FileOps::upgradeTarget(item.relativePath, item.replaces);
    if (target.isEmpty()) {
        const auto type = [](const QString &path) {
            const QString suffix = QFileInfo(path).suffix();
            return suffix.isEmpty() ? InboxService::tr("no type") : suffix.toUpper();
        };
        return judged(UpgradeVerdict::CantReplace,
                      InboxService::tr("The file it would replace is of another type (%1, not %2): Save to collection "
                                       "adds this one as a new file.")
                          .arg(type(item.replaces), type(item.relativePath)));
    }
    // An AAC, Ogg or Opus file's upgrade takes its name with .mp3 (D41): the note says so.
    const QString renamed = target == item.replaces ? QString() : QFileInfo(target).fileName();
    const TierInCollection now = tierOf ? tierOf(item.replaces) : TierInCollection{};
    switch (now.state) {
    case TierInCollection::State::NotReadYet:
        return judged(UpgradeVerdict::NotKnownYet, InboxService::tr("Selecta is still reading your collection: in a "
                                                                    "moment, it tells whether this one is better."));
    case TierInCollection::State::NotInCollection:
        // Its place is free while its folder is still the collection's: a file deleted, a replace cut short.
        if (folders.roleOf(item.replaces) == collection::FolderRoles::Role::None) {
            return judged(UpgradeVerdict::CantReplace,
                          InboxService::tr("The file isn't in your collection folders any more: Save to collection "
                                           "adds this one as a new file."));
        }
        return judged(UpgradeVerdict::Replaces,
                      renamed.isEmpty()
                          ? InboxService::tr("The file isn't in your collection any more: Save to collection puts "
                                             "this one in its place.")
                          : InboxService::tr("The file isn't in your collection any more: Save to collection puts "
                                             "this one in its place, as %1.")
                                .arg(renamed));
    case TierInCollection::State::InCollection:
        break;
    }
    const QualityTier arrived = downloadTier(item);
    if (arrived <= now.tier) {
        return judged(UpgradeVerdict::NotBetter,
                      InboxService::tr("Not better (%1; the file: %2): Save to collection adds it as a new file and "
                                       "leaves that file as it is.")
                          .arg(tierName(arrived), tierName(now.tier)));
    }
    return judged(UpgradeVerdict::Replaces,
                  renamed.isEmpty()
                      ? InboxService::tr("Better (%1; the file: %2): Save to collection puts it in that file's place, "
                                         "under the same name, and moves that file to the Trash.")
                            .arg(tierName(arrived), tierName(now.tier))
                      : InboxService::tr("Better (%1; the file: %2): Save to collection puts it in that file's place, "
                                         "as %3, and moves that file to the Trash.")
                            .arg(tierName(arrived), tierName(now.tier), renamed));
}

QString checkLibraryConfig(const LibraryConfig &config)
{
    if (config.inboxRoot.isEmpty() || !QDir::isAbsolutePath(config.inboxRoot)) {
        return InboxService::tr("Selecta's Inbox needs a full folder path.");
    }
    for (const QString &folder : {config.tracksRoot, config.albumsRoot}) {
        if (folder.isEmpty()) {
            continue; // not chosen yet
        }
        if (!QDir::isAbsolutePath(folder)) {
            return InboxService::tr("%1 isn't a full folder path.").arg(folder);
        }
        // Filing moves files out of the Inbox: a collection folder in it, or around it, would mix the two.
        if (overlap(folder, config.inboxRoot)) {
            return InboxService::tr("%1 holds Selecta's own Inbox, or is inside it. Choose another folder.")
                .arg(QDir::toNativeSeparators(folder));
        }
    }
    if (!config.tracksRoot.isEmpty() && !config.albumsRoot.isEmpty() && overlap(config.tracksRoot, config.albumsRoot)) {
        return collection::comparisonKey(config.tracksRoot) == collection::comparisonKey(config.albumsRoot)
                   ? InboxService::tr("The tracks folder and the albums folder must be two different folders.")
                   : InboxService::tr("The tracks folder and the albums folder can't be inside one another.");
    }
    return {};
}

InboxService::InboxService(storage::Database &db, LibraryConfig config, library::FileOps::TrashFunction trash,
                           QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_config(std::move(config))
    , m_trash(std::move(trash))
    , m_inbox(db)
    , m_history(db)
{
    qRegisterMetaType<InboxEntry>();
    qRegisterMetaType<TrashedItem>();
    m_pool.setMaxThreadCount(kFileWorkers);
    m_pool.setObjectName(u"selecta-files"_s);
    rebuildFileOps();
}

InboxService::~InboxService()
{
    waitForIdle();
}

void InboxService::waitForIdle()
{
    m_pool.waitForDone();
    drain();
}

void InboxService::setConfig(const LibraryConfig &config)
{
    m_config = config;
    rebuildFileOps();
    reconcile();
}

void InboxService::rebuildFileOps()
{
    // Work already running keeps the FileOps it started with; the shared pointer keeps it alive.
    m_fileOps = std::make_shared<const library::FileOps>(m_config.inboxRoot, m_config.folderRoles().roots(), m_trash);
}

QString InboxService::absolutePath(const InboxItem &item) const
{
    return QDir(m_config.inboxRoot).filePath(item.relativePath);
}

QList<InboxEntry> InboxService::entries()
{
    QList<InboxEntry> result;
    const QList<InboxItem> items = m_inbox.all();
    SupersedeFlagStore flags(m_db);
    const QHash<qint64, QStringList> flagged = flags.albumsByInboxItem();
    const QHash<qint64, QList<qint64>> flagIds = flags.idsByInboxItem();
    const collection::FolderRoles folders = m_config.folderRoles();
    result.reserve(items.size());
    for (const InboxItem &item : items) {
        UpgradeJudgement judged = judgeUpgrade(item, m_tierOf, folders);
        result.append(InboxEntry{.item = item,
                                 .busy = m_busy.contains(item.id),
                                 .alsoOn = flagged.value(item.id),
                                 .flags = flagIds.value(item.id),
                                 .upgrade = judged.verdict,
                                 .upgradeNote = std::move(judged.note)});
    }
    return result;
}

void InboxService::setTierLookup(TierLookup lookup)
{
    m_tierOf = std::move(lookup);
    publish();
}

void InboxService::publish()
{
    Q_EMIT changed(entries());
}

std::optional<qint64> InboxService::add(const library::InboxItem &item)
{
    const std::optional<qint64> id = m_inbox.add(item);
    if (id) {
        publish();
    }
    return id;
}

void InboxService::reconcile()
{
    const QList<InboxItem> items = m_inbox.all();
    for (const InboxItem &item : items) {
        if (m_busy.contains(item.id) || item.state == InboxItem::State::Processing) {
            continue;
        }
        if (!QFileInfo::exists(absolutePath(item))) {
            qCInfo(lcInbox) << "Inbox item" << item.relativePath << "is gone from disk; forgetting it";
            if (m_history.setWhereaboutsForInboxItem(item.id, Whereabouts::Trashed) < 0 || !m_inbox.remove(item.id)) {
                qCWarning(lcInbox) << "couldn't forget" << item.relativePath << ':' << m_db.lastError();
            }
        }
    }
    publish();
}

void InboxService::file(const QList<qint64> &ids)
{
    for (const qint64 id : ids) {
        const auto item = m_inbox.find(id);
        if (!item || m_busy.contains(id)) {
            continue;
        }
        if (item->state == InboxItem::State::Processing) {
            Q_EMIT finished(id, false, tr("%1 is still being converted.").arg(item->relativePath));
            continue;
        }
        const bool album = item->kind == InboxItem::Kind::Album;
        if (!album && !item->replaces.isEmpty()) {
            // Judged again now: the file it upgrades may have changed since the list said.
            const UpgradeVerdict verdict = judgeUpgrade(*item, m_tierOf, m_config.folderRoles()).verdict;
            const QString name = QFileInfo(item->relativePath).fileName();
            if (verdict == UpgradeVerdict::NotKnownYet) {
                Q_EMIT finished(id, false,
                                tr("Selecta is still reading your collection, to tell whether %1 is better: try again "
                                   "in a moment.")
                                    .arg(name));
                continue;
            }
            startUpgrade(id, absolutePath(*item), item->replaces, m_config.tracksRoot, name,
                         verdict != UpgradeVerdict::Replaces);
            continue;
        }
        const QString root = album ? m_config.albumsRoot : m_config.tracksRoot;
        if (root.isEmpty()) {
            Q_EMIT finished(id, false,
                            album ? tr("Choose your albums folder in Settings first.")
                                  : tr("Choose your tracks folder in Settings first."));
            continue;
        }
        const QString source = absolutePath(*item);
        const QString name = QFileInfo(item->relativePath).fileName();
        start(id, Work::File, [ops = m_fileOps, source, root, name, album] {
            return album ? ops->fileFolderIntoCollection(source, root, name)
                         : ops->fileIntoCollection(source, root, name);
        });
    }
    publish();
}

void InboxService::trash(const QList<qint64> &ids)
{
    for (const qint64 id : ids) {
        const auto item = m_inbox.find(id);
        if (!item || m_busy.contains(id)) {
            continue;
        }
        start(id, Work::Trash, [ops = m_fileOps, path = absolutePath(*item)] { return ops->trashFromInbox(path); });
    }
    publish();
}

void InboxService::restore(const TrashedItem &trashed)
{
    if (trashed.trashedPath.isEmpty() || !QFileInfo::exists(trashed.trashedPath)) {
        Q_EMIT finished(trashed.item.id, false,
                        tr("%1 is no longer in the Trash.").arg(QFileInfo(trashed.item.relativePath).fileName()));
        return;
    }
    const bool album = trashed.item.kind == InboxItem::Kind::Album;
    m_pool.start([this, trashed, album, ops = m_fileOps] {
        post(Outcome{.id = trashed.item.id,
                     .work = Work::Restore,
                     .result = album ? ops->publishFolderIntoInbox(trashed.trashedPath, trashed.item.relativePath)
                                     : ops->publishIntoInbox(trashed.trashedPath, trashed.item.relativePath),
                     .retag = {},
                     .tagSource = {},
                     .releaseJson = {},
                     .restored = trashed});
    });
}

void InboxService::restored(const Outcome &outcome)
{
    const QString name = QFileInfo(outcome.restored.item.relativePath).fileName();
    if (!outcome.result.ok) {
        qCWarning(lcInbox).noquote() << "couldn't put" << name << "back:" << outcome.result.error;
        Q_EMIT finished(outcome.id, false, outcome.result.error);
        return;
    }
    InboxItem item = outcome.restored.item;
    const QString inboxRoot = QFileInfo(m_config.inboxRoot).canonicalFilePath();
    item.relativePath = QDir(inboxRoot.isEmpty() ? m_config.inboxRoot : inboxRoot)
                            .relativeFilePath(QFileInfo(outcome.result.path).canonicalFilePath());
    item.id = 0;
    const std::optional<qint64> id = m_inbox.add(item);
    if (!id) {
        // The file is back; the next reconcile() can't know it, but the user sees it in the Inbox folder.
        qCWarning(lcInbox) << "put" << item.relativePath << "back but couldn't record it:" << m_db.lastError();
        Q_EMIT finished(outcome.id, false,
                        tr("%1 is back in your Inbox folder, but Selecta couldn't list it.").arg(name));
        return;
    }
    if (m_history.returnToInbox(outcome.restored.history, *id) < 0) {
        qCWarning(lcInbox) << "couldn't update the history of" << item.relativePath << ':' << m_db.lastError();
    }
    qCInfo(lcInbox).noquote() << "put back" << item.relativePath << "from the Trash";
    Q_EMIT finished(*id, true, tr("%1 is back in your Inbox").arg(name));
}

void InboxService::retag(qint64 id, const convert::MetadataModel &changes, const QStringList &cleared,
                         const QString &tagSource, const QString &releaseJson)
{
    const auto item = m_inbox.find(id);
    if (!item) {
        return;
    }
    if (m_busy.contains(id) || item->state == InboxItem::State::Processing) {
        Q_EMIT finished(id, false, tr("%1 is busy; try again in a moment.").arg(item->relativePath));
        return;
    }
    if (item->kind != InboxItem::Kind::Single) {
        Q_EMIT finished(id, false, tr("Albums can't be re-tagged as a whole."));
        return;
    }
    RetagRequest request;
    request.path = absolutePath(*item);
    request.inboxRoot = m_config.inboxRoot;
    request.templates = m_config.templates;
    request.cleaner = m_config.cleaner;
    request.changes = changes;
    request.cleared = cleared;
    m_busy.insert(id);
    m_pool.start([this, id, request = std::move(request), ops = m_fileOps, tagSource, releaseJson] {
        post(Outcome{.id = id,
                     .work = Work::Retag,
                     .result = {},
                     .retag = retagInboxFile(request, *ops),
                     .tagSource = tagSource,
                     .releaseJson = releaseJson,
                     .restored = {}});
    });
    publish();
}

void InboxService::start(qint64 id, Work work, std::function<FileResult()> job)
{
    m_busy.insert(id);
    m_pool.start([this, id, work, job = std::move(job)] {
        post(Outcome{
            .id = id, .work = work, .result = job(), .retag = {}, .tagSource = {}, .releaseJson = {}, .restored = {}});
    });
}

void InboxService::startUpgrade(qint64 id, const QString &source, const QString &replaces, const QString &tracksRoot,
                                const QString &name, bool asNew)
{
    m_busy.insert(id);
    m_pool.start([this, id, ops = m_fileOps, source, replaces, tracksRoot, name, asNew] {
        Outcome outcome{.id = id, .work = Work::Replace};
        if (!asNew) {
            const library::FileResult check = ops->checkReplace(source, replaces);
            if (check.ok) {
                // Its real path, as History keeps filed paths: its downloads go to the Trash with it.
                const QFileInfo old(replaces);
                const QString real = old.canonicalFilePath();
                outcome.upgraded = real.isEmpty() ? QDir::cleanPath(replaces) : real;
                outcome.oldThere = old.exists();
                outcome.result = ops->replaceInCollection(source, replaces);
            } else if (!QFileInfo(QFileInfo(replaces).absolutePath()).isDir()) {
                outcome.result = library::FileResult::failure(
                    tr("The folder of %1 isn't there: plug its drive in and try again.")
                        .arg(QDir::toNativeSeparators(replaces)));
            } else {
                outcome.result = library::FileResult::failure(
                    tr("It can't take the place of %1: %2").arg(QDir::toNativeSeparators(replaces), check.error));
            }
        } else {
            // Not to take its file's place (the Inbox said why): a new file, like any single; the file stays.
            outcome.work = Work::File;
            outcome.upgraded = replaces;
            outcome.result = tracksRoot.isEmpty()
                                 ? library::FileResult::failure(tr("Choose your tracks folder in Settings first."))
                                 : ops->fileIntoCollection(source, tracksRoot, name);
        }
        post(std::move(outcome));
    });
}

void InboxService::post(Outcome outcome)
{
    {
        const std::scoped_lock lock(m_outcomesMutex);
        m_outcomes.append(std::move(outcome));
    }
    // The destructor waits for the pool before `this` goes, and drains whatever this call doesn't get to.
    QMetaObject::invokeMethod(this, &InboxService::drain, Qt::QueuedConnection);
}

void InboxService::retagged(const Outcome &outcome)
{
    std::optional<InboxItem> item = m_inbox.find(outcome.id);
    if (!outcome.retag.ok || !item) {
        qCWarning(lcInbox).noquote() << "couldn't re-tag" << outcome.id << ':' << outcome.retag.error;
        Q_EMIT finished(outcome.id, false, outcome.retag.error);
        return;
    }
    namespace keys = convert::keys;
    const convert::MetadataModel &tags = outcome.retag.tags;
    const QString inboxRoot = QFileInfo(m_config.inboxRoot).canonicalFilePath();
    item->relativePath = QDir(inboxRoot.isEmpty() ? m_config.inboxRoot : inboxRoot)
                             .relativeFilePath(QFileInfo(outcome.retag.path).canonicalFilePath());
    item->artist = tags.values(keys::Artist).join(u", "_s);
    item->title = tags.value(keys::Title);
    item->album = tags.value(keys::Album);
    bool yearOk = false;
    const int year = tags.value(keys::Date).left(4).toInt(&yearOk);
    item->year = yearOk && year > 0 ? std::optional<int>(year) : std::nullopt;
    item->sizeBytes = outcome.retag.sizeBytes;
    if (!outcome.tagSource.isEmpty()) {
        item->tagSource = outcome.tagSource;
    }
    if (!outcome.releaseJson.isEmpty()) {
        item->releaseJson = outcome.releaseJson;
    }
    if (!m_inbox.update(*item)) {
        // The file has its new tags; the list catches up at the next reconcile().
        qCWarning(lcInbox) << "re-tagged" << item->relativePath << "but couldn't update the Inbox:" << m_db.lastError();
    }
    qCInfo(lcInbox).noquote() << "re-tagged" << item->relativePath;
    Q_EMIT finished(outcome.id, true, tr("Saved the tags of %1").arg(QFileInfo(item->relativePath).fileName()));
}

void InboxService::drain()
{
    Q_ASSERT(QThread::currentThread() == thread());
    QList<Outcome> outcomes;
    {
        const std::scoped_lock lock(m_outcomesMutex);
        outcomes.swap(m_outcomes);
    }
    if (outcomes.isEmpty()) {
        return;
    }
    for (const Outcome &outcome : std::as_const(outcomes)) {
        m_busy.remove(outcome.id);
        if (outcome.work == Work::Retag) {
            retagged(outcome);
            continue;
        }
        if (outcome.work == Work::Restore) {
            restored(outcome);
            continue;
        }
        const auto item = m_inbox.find(outcome.id);
        const QString name = item ? item->relativePath : QString::number(outcome.id);
        if (!outcome.result.ok) {
            qCWarning(lcInbox).noquote() << "couldn't" << (outcome.work == Work::Trash ? "trash" : "file") << name
                                         << ':' << outcome.result.error;
            Q_EMIT finished(outcome.id, false, outcome.result.error);
            continue;
        }
        const bool replaced = outcome.work == Work::Replace;
        const bool filed = outcome.work == Work::File || replaced;
        if (replaced) {
            // The old file's downloads went to the Trash with it; so did any earlier file of the path it takes (.mp3).
            const int old = m_history.trashFiledAt(outcome.upgraded);
            if (const int earlier = m_history.trashFiledAt(outcome.result.path); old < 0 || earlier < 0) {
                qCWarning(lcInbox) << "couldn't record that the replaced file went to the Trash:" << m_db.lastError();
            }
        }
        const QList<qint64> history = filed ? QList<qint64>() : m_history.idsForInboxItem(outcome.id);
        const int recorded = m_history.setWhereaboutsForInboxItem(
            outcome.id, filed ? Whereabouts::Filed : Whereabouts::Trashed, filed ? outcome.result.path : QString());
        if (recorded < 0 || !m_inbox.remove(outcome.id)) {
            // The file already moved; reconcile() will drop the stale row on the next pass.
            qCWarning(lcInbox) << "moved" << name << "but couldn't update the Inbox:" << m_db.lastError();
        }
        const char *done = "trashed";
        if (replaced) {
            done = "replaced with";
        } else if (filed) {
            done = "filed";
        }
        qCInfo(lcInbox).noquote() << done << name << "→" << outcome.result.path;
        QString message;
        if (replaced && !outcome.oldThere) {
            message = tr("Filed to %1, the place of the file it upgrades, which wasn't there any more.")
                          .arg(outcome.result.path);
        } else if (replaced && QFileInfo(outcome.upgraded).fileName() != QFileInfo(outcome.result.path).fileName()) {
            message = tr("Replaced %1 with %2. The old file is in the Trash.")
                          .arg(QDir::toNativeSeparators(outcome.upgraded), outcome.result.path);
        } else if (replaced) {
            message = tr("Replaced %1. The old file is in the Trash.").arg(outcome.result.path);
        } else if (filed && !outcome.upgraded.isEmpty()) {
            message = tr("Filed to %1 as a new file; %2 stays as it was.")
                          .arg(outcome.result.path, QDir::toNativeSeparators(outcome.upgraded));
        } else {
            message = filed ? tr("Filed to %1").arg(outcome.result.path) : tr("Moved %1 to the Trash").arg(name);
        }
        Q_EMIT finished(outcome.id, true, message);
        if (!filed && item) {
            Q_EMIT trashed(TrashedItem{.item = *item, .trashedPath = outcome.result.path, .history = history});
        }
    }
    publish();
}

} // namespace selecta::core
