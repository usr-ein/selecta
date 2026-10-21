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

QString upgradeMismatch(const library::InboxItem &item)
{
    if (item.replaces.isEmpty()) {
        return {};
    }
    if (item.kind != library::InboxItem::Kind::Single) {
        return InboxService::tr("An album can't replace a track.");
    }
    const QString mine = QFileInfo(item.relativePath).suffix();
    const QString theirs = QFileInfo(item.replaces).suffix();
    if (mine.compare(theirs, Qt::CaseInsensitive) == 0) {
        return {};
    }
    const auto type = [](const QString &suffix) {
        return suffix.isEmpty() ? InboxService::tr("no type") : suffix.toUpper();
    };
    return InboxService::tr("The file it would replace is of another type (%1, not %2): Save to collection adds this "
                            "one as a new file.")
        .arg(type(theirs), type(mine));
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
    result.reserve(items.size());
    for (const InboxItem &item : items) {
        result.append(InboxEntry{.item = item,
                                 .busy = m_busy.contains(item.id),
                                 .alsoOn = flagged.value(item.id),
                                 .flags = flagIds.value(item.id)});
    }
    return result;
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
            startUpgrade(id, absolutePath(*item), item->replaces, m_config.tracksRoot,
                         QFileInfo(item->relativePath).fileName());
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
                                const QString &name)
{
    m_busy.insert(id);
    m_pool.start([this, id, ops = m_fileOps, source, replaces, tracksRoot, name] {
        Outcome outcome{.id = id, .work = Work::Replace};
        const library::FileResult check = ops->checkReplace(source, replaces);
        if (check.ok) {
            outcome.result = ops->replaceInCollection(source, replaces);
        } else {
            // It can't take the file's place: a new file, like any single, and the message says why.
            outcome.work = Work::File;
            outcome.note = check.error;
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
            qCWarning(lcInbox).noquote() << "couldn't" << (outcome.work == Work::Trash ? "trash" : "file") << name << ':'
                                         << outcome.result.error;
            Q_EMIT finished(outcome.id, false, outcome.result.error);
            continue;
        }
        const bool replaced = outcome.work == Work::Replace;
        const bool filed = outcome.work == Work::File || replaced;
        if (replaced && m_history.trashFiledAt(outcome.result.path) < 0) {
            qCWarning(lcInbox) << "couldn't record that the replaced file went to the Trash:" << m_db.lastError();
        }
        const QList<qint64> history = filed ? QList<qint64>() : m_history.idsForInboxItem(outcome.id);
        const int recorded = m_history.setWhereaboutsForInboxItem(
            outcome.id, filed ? Whereabouts::Filed : Whereabouts::Trashed, filed ? outcome.result.path : QString());
        if (recorded < 0 || !m_inbox.remove(outcome.id)) {
            // The file already moved; reconcile() will drop the stale row on the next pass.
            qCWarning(lcInbox) << "moved" << name << "but couldn't update the Inbox:" << m_db.lastError();
        }
        qCInfo(lcInbox).noquote() << (replaced ? "replaced with" : filed ? "filed" : "trashed") << name << "→"
                                  << outcome.result.path;
        QString message;
        if (replaced) {
            message = tr("Replaced %1. The old file is in the Trash.").arg(outcome.result.path);
        } else if (filed && !outcome.note.isEmpty()) {
            message = tr("Filed to %1 as a new file: it couldn't replace the old one (%2).")
                          .arg(outcome.result.path, outcome.note);
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
