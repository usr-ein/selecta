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

// Whether one of the two folders is the other or inside it.
bool overlap(const QString &first, const QString &second)
{
    const QString a = QDir::cleanPath(first);
    const QString b = QDir::cleanPath(second);
    return a == b || a.startsWith(b + u'/') || b.startsWith(a + u'/');
}

} // namespace

QString checkLibraryConfig(const LibraryConfig &config)
{
    if (config.inboxRoot.isEmpty() || !QDir::isAbsolutePath(config.inboxRoot)) {
        return InboxService::tr("Choose a folder for the Inbox.");
    }
    for (qsizetype i = 0; i < config.collectionRoots.size(); ++i) {
        const QString &root = config.collectionRoots.at(i);
        if (root.isEmpty() || !QDir::isAbsolutePath(root)) {
            return InboxService::tr("%1 isn't a full folder path.").arg(root);
        }
        if (overlap(config.inboxRoot, root)) {
            return InboxService::tr("The Inbox and the collection folder %1 can't be inside one another.").arg(root);
        }
        for (qsizetype j = i + 1; j < config.collectionRoots.size(); ++j) {
            const QString &other = config.collectionRoots.at(j);
            if (overlap(root, other)) {
                return InboxService::tr("The collection folders %1 and %2 overlap.").arg(root, other);
            }
        }
    }
    if (config.singlesFolder.isEmpty() || config.albumsFolder.isEmpty() || config.singlesFolder.contains(u'/') ||
        config.albumsFolder.contains(u'/')) {
        return InboxService::tr("The Singles and Albums folders need plain names.");
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
    m_fileOps = std::make_shared<const library::FileOps>(m_config.inboxRoot, m_config.collectionRoots, m_trash);
}

QString InboxService::absolutePath(const InboxItem &item) const
{
    return QDir(m_config.inboxRoot).filePath(item.relativePath);
}

QList<InboxEntry> InboxService::entries()
{
    QList<InboxEntry> result;
    const QList<InboxItem> items = m_inbox.all();
    const QHash<qint64, QStringList> flagged = SupersedeFlagStore(m_db).albumsByInboxItem();
    result.reserve(items.size());
    for (const InboxItem &item : items) {
        result.append(InboxEntry{.item = item, .busy = m_busy.contains(item.id), .alsoOn = flagged.value(item.id)});
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
        if (m_config.collectionRoots.isEmpty()) {
            Q_EMIT finished(id, false, tr("Choose your collection folder in Settings first."));
            continue;
        }
        const QString root = m_config.collectionRoots.first();
        const QString source = absolutePath(*item);
        const QString name = QFileInfo(item->relativePath).fileName();
        const bool album = item->kind == InboxItem::Kind::Album;
        const QString relative = (album ? m_config.albumsFolder : m_config.singlesFolder) + u'/' + name;
        start(id, Work::File, [ops = m_fileOps, source, root, relative, album] {
            return album ? ops->fileFolderIntoCollection(source, root, relative)
                         : ops->fileIntoCollection(source, root, relative);
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
            qCWarning(lcInbox).noquote() << "couldn't" << (outcome.work == Work::File ? "file" : "trash") << name << ':'
                                         << outcome.result.error;
            Q_EMIT finished(outcome.id, false, outcome.result.error);
            continue;
        }
        const bool filed = outcome.work == Work::File;
        const QList<qint64> history = filed ? QList<qint64>() : m_history.idsForInboxItem(outcome.id);
        const int recorded = m_history.setWhereaboutsForInboxItem(
            outcome.id, filed ? Whereabouts::Filed : Whereabouts::Trashed, filed ? outcome.result.path : QString());
        if (recorded < 0 || !m_inbox.remove(outcome.id)) {
            // The file already moved; reconcile() will drop the stale row on the next pass.
            qCWarning(lcInbox) << "moved" << name << "but couldn't update the Inbox:" << m_db.lastError();
        }
        qCInfo(lcInbox).noquote() << (filed ? "filed" : "trashed") << name << "→" << outcome.result.path;
        Q_EMIT finished(outcome.id, true,
                        filed ? tr("Filed to %1").arg(outcome.result.path) : tr("Moved %1 to the Trash").arg(name));
        if (!filed && item) {
            Q_EMIT trashed(TrashedItem{.item = *item, .trashedPath = outcome.result.path, .history = history});
        }
    }
    publish();
}

} // namespace selecta::core
