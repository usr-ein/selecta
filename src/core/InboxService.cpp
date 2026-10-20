// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxService.h"

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
    result.reserve(items.size());
    for (const InboxItem &item : items) {
        result.append(InboxEntry{.item = item, .busy = m_busy.contains(item.id)});
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

void InboxService::start(qint64 id, Work work, std::function<FileResult()> job)
{
    m_busy.insert(id);
    m_pool.start([this, id, work, job = std::move(job)] {
        Outcome outcome{.id = id, .work = work, .result = job()};
        {
            const std::scoped_lock lock(m_outcomesMutex);
            m_outcomes.append(std::move(outcome));
        }
        // The destructor waits for the pool before `this` goes, and drains whatever this call doesn't get to.
        QMetaObject::invokeMethod(this, &InboxService::drain, Qt::QueuedConnection);
    });
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
        const auto item = m_inbox.find(outcome.id);
        const QString name = item ? item->relativePath : QString::number(outcome.id);
        if (!outcome.result.ok) {
            qCWarning(lcInbox).noquote() << "couldn't" << (outcome.work == Work::File ? "file" : "trash") << name << ':'
                                         << outcome.result.error;
            Q_EMIT finished(outcome.id, false, outcome.result.error);
            continue;
        }
        const bool filed = outcome.work == Work::File;
        const int recorded = m_history.setWhereaboutsForInboxItem(
            outcome.id, filed ? Whereabouts::Filed : Whereabouts::Trashed, filed ? outcome.result.path : QString());
        if (recorded < 0 || !m_inbox.remove(outcome.id)) {
            // The file already moved; reconcile() will drop the stale row on the next pass.
            qCWarning(lcInbox) << "moved" << name << "but couldn't update the Inbox:" << m_db.lastError();
        }
        qCInfo(lcInbox).noquote() << (filed ? "filed" : "trashed") << name << "→" << outcome.result.path;
        Q_EMIT finished(outcome.id, true,
                        filed ? tr("Filed to %1").arg(outcome.result.path) : tr("Moved %1 to the Trash").arg(name));
    }
    publish();
}

} // namespace selecta::core
