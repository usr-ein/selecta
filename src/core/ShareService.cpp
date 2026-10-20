// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ShareService.h"

#include "base/Version.h"
#include "core/ActivityLog.h"
#include "core/CollectionStore.h"
#include "core/NetworkService.h"
#include "governor/Governor.h"
#include "share/Open.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QFileInfo>
#include <QIODevice>
#include <QLoggingCategory>
#include <QTimer>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcShares, "selecta.core.shares")

constexpr auto kTick = 10s;
constexpr int kTicksPerTotals = 6; // the ratio is read again every minute
constexpr qint64 kDayMs = qint64{24} * 60 * 60 * 1000;

using storage::Statement;

QString outcomeOf(slsk::UploadState state)
{
    switch (state) {
    case slsk::UploadState::Completed:
        return u"completed"_s;
    case slsk::UploadState::Cancelled:
        return u"cancelled"_s;
    case slsk::UploadState::Declined:
        return u"declined"_s;
    default:
        break;
    }
    return u"failed"_s;
}

TransferTotals totals(storage::Database &db, const QString &sql, qint64 sinceMs, std::optional<qint64> session)
{
    Statement query = db.prepare(sql);
    if (!query.isValid()) {
        return {};
    }
    query.bind(1, sinceMs);
    if (session) {
        query.bind(2, *session);
    } else {
        query.bindNull(2);
    }
    if (query.step() != Statement::Step::Row) {
        return {};
    }
    return {.files = static_cast<int>(query.int64(0)), .bytes = query.int64(1)};
}

} // namespace

TransferTotals uploadTotals(storage::Database &db, qint64 sinceMs, std::optional<qint64> session)
{
    // Files count once they arrived whole; bytes count as they went out, broken uploads included.
    return totals(db,
                  u"SELECT COALESCE(SUM(outcome = 'completed'), 0), COALESCE(SUM(sent), 0) FROM uploads "
                  "WHERE ended_ms >= ?1 AND (?2 IS NULL OR session = ?2);"_s,
                  sinceMs, session);
}

TransferTotals downloadTotals(storage::Database &db, qint64 sinceMs, std::optional<qint64> session)
{
    return totals(db,
                  u"SELECT COUNT(*), COALESCE(SUM(size_bytes), 0) FROM history "
                  "WHERE kind IN ('single', 'album') AND completed_at >= ?1 AND (?2 IS NULL OR session = ?2);"_s,
                  sinceMs, session);
}

RatioAssessment assessRatio(const ShareStatus &status)
{
    constexpr qint64 kLittleTaken = qint64{1} * 1000 * 1000 * 1000; // 1 GB in 30 days: nobody minds yet
    const auto size = [](qint64 bytes) {
        return bytes >= 1'000'000'000 ? u"%1 GB"_s.arg(static_cast<double>(bytes) / 1e9, 0, 'f', 1)
                                      : u"%1 MB"_s.arg(static_cast<double>(bytes) / 1e6, 0, 'f', 0);
    };
    RatioAssessment assessment;
    const TransferTotals &up = status.uploaded30Days;
    const TransferTotals &down = status.downloaded30Days;
    assessment.summary = u"↑ %1 · ↓ %2"_s.arg(size(up.bytes), size(down.bytes));
    if (status.files == 0) {
        assessment.level = RatioAssessment::Level::NotSharing;
        assessment.guidance = ShareService::tr("You share nothing yet. Choose your tracks and albums folders in "
                                               "Settings → Library: Selecta shares them, read-only, so others can get "
                                               "from you too.");
        return assessment;
    }
    if (down.bytes <= kLittleTaken || up.bytes * 4 >= down.bytes) {
        assessment.level = RatioAssessment::Level::Healthy;
        assessment.guidance =
            ShareService::tr("Healthy: you share %1 files and give back as you take.").arg(status.files);
        return assessment;
    }
    assessment.level = RatioAssessment::Level::Low;
    assessment.guidance = ShareService::tr("Low: in 30 days you took %1 and gave back %2. Leave Selecta running "
                                           "connected so others can download from you, and spread your downloads "
                                           "over more peers.")
                              .arg(size(down.bytes), size(up.bytes));
    return assessment;
}

ShareService::ShareService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                           ActivityLog &activity, const base::Clock &clock, std::optional<qint64> session,
                           WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_activity(activity)
    , m_session(session)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_index(std::make_shared<const share::ShareIndex>())
    , m_answers(clock, kAnswersBurst, std::chrono::duration_cast<base::Clock::Duration>(60s) / kAnswersPerMinute)
    , m_rebuildTimer(new QTimer(this))
    , m_tick(new QTimer(this))
{
    qRegisterMetaType<ShareStatus>();
    qRegisterMetaType<UploadEntry>();
    qRegisterMetaType<QList<UploadEntry>>();
    m_pool.setMaxThreadCount(1);
    m_pool.setObjectName(u"selecta-shares"_s);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(kRebuildDelayMs);
    connect(m_rebuildTimer, &QTimer::timeout, this, &ShareService::rebuild);
    m_tick->setInterval(kTick);
    connect(m_tick, &QTimer::timeout, this, [this, ticks = 0]() mutable {
        startUploads();
        if (++ticks % kTicksPerTotals == 0) {
            refreshTotals();
        }
    });
    m_tick->start();

    m_network.peers().setShareProvider(&m_shares);
    m_network.peers().setUserInfoProvider(&m_info);
    connect(&m_network, &NetworkService::searchRequestReceived, this, &ShareService::searchReceived);
    connect(&m_network, &NetworkService::privilegedUsersReceived, this, [this](const QStringList &users) {
        m_privileged = QSet<QString>(users.cbegin(), users.cend());
        m_queue.setPrivileged(m_privileged);
    });
    const slsk::TransferManager &transfers = m_network.transfers();
    connect(&transfers, &slsk::TransferManager::uploadRequested, this, &ShareService::uploadRequested);
    connect(&transfers, &slsk::TransferManager::placeInQueueRequested, this, &ShareService::placeInQueueRequested);
    connect(&transfers, &slsk::TransferManager::uploadChanged, this, &ShareService::uploadChanged);
    connect(&transfers, &slsk::TransferManager::uploadProgress, this, &ShareService::uploadProgress);
    connect(&m_governor, &governor::Governor::pausedChanged, this, [this](bool paused) {
        if (!paused) {
            startUploads();
        }
    });
    refreshTotals();
}

ShareService::~ShareService()
{
    m_network.peers().setShareProvider(nullptr);
    m_network.peers().setUserInfoProvider(nullptr);
    // Uploads under way stop (the downloaders may queue again later); their devices go after the manager lets go.
    QList<slsk::TransferId> running;
    running.reserve(m_active.size());
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it) {
        running.append(it.key());
    }
    for (const slsk::TransferId id : std::as_const(running)) {
        m_network.transfers().cancelUpload(id);
    }
    m_active.clear();
    if (m_cancel) {
        m_cancel->cancel();
    }
    m_pool.waitForDone();
}

void ShareService::setRoots(const QStringList &roots, const QStringList &excluded)
{
    m_roots = roots;
    m_excluded = excluded;
    m_status.roots = roots;
    rebuild();
}

void ShareService::collectionChanged()
{
    m_rebuildTimer->start();
}

void ShareService::rebuild()
{
    m_rebuildTimer->stop();
    if (m_building) {
        m_rebuildWanted = true;
        if (m_cancel) {
            m_cancel->cancel(); // what it read is stale already
        }
        return;
    }
    QList<share::ShareRoot> roots;
    for (const QString &root : std::as_const(m_roots)) {
        roots.append(share::ShareRoot{.path = root, .alias = QFileInfo(root).fileName()});
    }
    // The collection scan already read every audio file's facts: the build reuses them instead of probing.
    QHash<QString, collection::ScannedFile> known;
    const QList<CollectionFile> files = CollectionStore(m_db).all();
    known.reserve(files.size());
    for (const CollectionFile &file : files) {
        known.insert(file.file.file.path, file.file);
    }
    m_building = true;
    m_status.building = true;
    m_cancel = std::make_shared<audio::CancellationFlag>();
    const quint64 generation = ++m_generation;
    publish();
    m_pool.start([this, roots = std::move(roots), known = std::move(known), excluded = m_excluded, cancel = m_cancel,
                  generation] {
        Built built{.result = share::build(roots, known, {.excludedFolders = excluded}, cancel.get()),
                    .generation = generation};
        {
            const std::scoped_lock lock(m_mutex);
            m_built.append(std::move(built));
        }
        // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    });
}

void ShareService::drain()
{
    QList<Built> built;
    {
        const std::scoped_lock lock(m_mutex);
        built.swap(m_built);
    }
    for (const Built &one : std::as_const(built)) {
        if (one.generation != m_generation) {
            continue;
        }
        m_building = false;
        m_status.building = false;
        if (!one.result.cancelled) {
            const share::ShareIndex::Counts before = m_index->counts();
            m_index = one.result.index;
            const share::ShareIndex::Counts counts = m_index->counts();
            m_status.folders = counts.folders;
            m_status.files = counts.files;
            m_status.message.clear();
            if (!one.result.unreadable.isEmpty()) {
                const auto &[path, reason] = one.result.unreadable.constFirst();
                m_status.message = one.result.unreadable.size() == 1
                                       ? tr("Not shared: %1 (%2)").arg(path, reason)
                                       : tr("%1 files or folders can't be shared, %2 for one: %3")
                                             .arg(one.result.unreadable.size())
                                             .arg(path, reason);
            }
            if (counts != before) {
                m_network.setSharedCounts(static_cast<quint32>(counts.folders), static_cast<quint32>(counts.files));
            }
            qCInfo(lcShares) << "sharing" << counts.files << "files in" << counts.folders << "folders";
        }
        publish();
        if (std::exchange(m_rebuildWanted, false)) {
            rebuild();
        }
    }
}

bool ShareService::isPaused() const
{
    return m_governor.isPaused();
}

slsk::Client::UploadAvailability ShareService::availability() const
{
    return {.slotFree = m_queue.slotFree(),
            .averageSpeed = 0,
            .queueLength = static_cast<quint32>(std::max(m_queue.queued(), 0))};
}

void ShareService::searchReceived(const slsk::IncomingSearch &search)
{
    if (isPaused() || search.username.isEmpty() || search.username == m_network.username() ||
        m_banned.contains(search.username)) {
        return; // Soulfind echoes our own searches back to us; banned users get nothing (NET-8)
    }
    const QList<slsk::FileEntry> results = m_index->search(search.query, m_governor.excludedPhrases());
    if (results.isEmpty() || !m_answers.tryTake()) {
        return;
    }
    if (m_network.answerSearch(search, results, availability())) {
        ++m_status.searchesAnswered;
    }
}

QByteArray ShareService::sharedFileList(const QString &username)
{
    if (isPaused() || m_banned.contains(username)) {
        static const share::ShareIndex nothing;
        return nothing.sharedFileList();
    }
    ++m_status.browsed;
    m_activity.record(u"shares"_s, username, u"browsed"_s, tr("%1 looked at what you share").arg(username));
    return m_index->sharedFileList();
}

QList<slsk::SharedDirectory> ShareService::folderContents(const QString &username, const slsk::RemotePath &folder)
{
    return isPaused() || m_banned.contains(username) ? QList<slsk::SharedDirectory>() : m_index->folderContents(folder);
}

slsk::peer::UserInfoResponse ShareService::userInfo(const QString &username)
{
    Q_UNUSED(username)
    slsk::peer::UserInfoResponse info;
    info.description = u"Selecta %1"_s.arg(QString(base::kVersion));
    info.totalUploads = static_cast<quint32>(std::max(m_queue.options().uploadSlots, 0));
    info.queueSize = static_cast<quint32>(std::max(m_queue.queued(), 0));
    info.slotsFree = m_queue.slotFree();
    return info;
}

void ShareService::uploadRequested(const QString &user, const slsk::RemotePath &path, bool legacy)
{
    Q_UNUSED(legacy) // slsk already answered a legacy request "Queued"
    slsk::TransferManager &transfers = m_network.transfers();
    if (m_banned.contains(user)) {
        transfers.denyUpload(user, path, u"Banned"_s); // the standard denial (NET-8)
        m_activity.record(u"upload"_s, user, u"denied"_s, u"Banned"_s);
        return;
    }
    const std::optional<share::SharedFile> file = m_index->lookup(path);
    if (!file) {
        transfers.denyUpload(user, path, u"File not shared."_s);
        return;
    }
    const share::UploadQueue::Decision decision = m_queue.enqueue(user, path, file->size, m_privileged.contains(user));
    if (!decision.accepted) {
        transfers.denyUpload(user, path, decision.reason);
        m_activity.record(u"upload"_s, user, u"denied"_s, decision.reason);
        return;
    }
    if (!decision.repeat) {
        m_activity.record(u"upload"_s, user, u"queued"_s, path.display());
    }
    startUploads();
    publish();
    publishUploads();
}

void ShareService::setBanned(const QSet<QString> &banned)
{
    const QSet<QString> added = banned - m_banned;
    m_banned = banned;
    // Newly banned users leave the queue at once (their transfers in flight finish: cutting them helps nobody).
    for (const QString &user : added) {
        const QList<slsk::RemotePath> paths = m_queue.removeUser(user);
        for (const slsk::RemotePath &path : paths) {
            m_network.transfers().denyUpload(user, path, u"Banned"_s);
        }
    }
    if (!added.isEmpty()) {
        publish();
        publishUploads();
    }
}

void ShareService::placeInQueueRequested(const QString &user, const slsk::RemotePath &path)
{
    if (const std::optional<quint32> place = m_queue.placeInQueue(user, path)) {
        m_network.transfers().sendPlaceInQueue(user, path, *place);
    }
}

void ShareService::startUploads()
{
    if (isPaused() || !m_network.isOnline()) {
        return;
    }
    slsk::TransferManager &transfers = m_network.transfers();
    bool started = false;
    // next() changes nothing: each pass either starts the file or takes it out of the queue.
    while (const std::optional<share::UploadQueue::Next> next = m_queue.next()) {
        const std::optional<share::SharedFile> file = m_index->lookup(next->path);
        if (!file) {
            m_queue.remove(next->user, next->path);
            transfers.denyUpload(next->user, next->path, u"File not shared."_s);
            continue;
        }
        share::Opened opened = share::open(*file);
        if (!opened.ok()) {
            qCInfo(lcShares).noquote() << "can't upload" << file->localPath << ':' << opened.detail;
            m_queue.remove(next->user, next->path);
            transfers.denyUpload(next->user, next->path, opened.error);
            continue;
        }
        const slsk::TransferId id = transfers.startUpload(
            slsk::UploadRequest{.username = next->user, .path = next->path, .size = file->size}, opened.device.get());
        if (id == 0) {
            m_queue.remove(next->user, next->path);
            continue;
        }
        m_queue.started(next->user, next->path);
        Active active;
        active.device = std::move(opened.device);
        active.user = next->user;
        active.path = next->path;
        active.size = file->size;
        active.startedMs = m_now();
        m_active.insert(id, std::make_shared<Active>(std::move(active)));
        m_activity.record(u"upload"_s, next->user, u"started"_s, next->path.display());
        started = true;
    }
    if (started) {
        publish();
        publishUploads();
    }
}

void ShareService::uploadProgress(slsk::TransferId id, quint64 sent, quint64 total)
{
    Q_UNUSED(total)
    if (const auto found = m_active.find(id); found != m_active.end()) {
        found.value()->sent = sent;
    }
}

void ShareService::uploadChanged(const slsk::UploadInfo &info)
{
    const auto found = m_active.find(info.id);
    if (found == m_active.end()) {
        return;
    }
    found.value()->sent = std::max(found.value()->sent, info.startOffset + info.sent);
    if (!info.isTerminal()) {
        publishUploads();
        return;
    }
    // Terminal: the manager is done with the device; it goes with the entry.
    const std::shared_ptr<Active> upload = std::move(found.value()); // kept alive past the erase
    m_active.erase(found);
    m_queue.finished(upload->user, upload->path);
    record(*upload, info);
    m_activity.record(u"upload"_s, upload->user, outcomeOf(info.state), upload->path.display());
    startUploads();
    refreshTotals();
    publishUploads();
}

void ShareService::record(const Active &upload, const slsk::UploadInfo &info)
{
    Statement insert = m_db.prepare(u"INSERT INTO uploads (session, username, path, size, sent, started_ms, ended_ms, "
                                    "outcome) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);"_s);
    if (!insert.isValid()) {
        return;
    }
    if (m_session) {
        insert.bind(1, *m_session);
    } else {
        insert.bindNull(1);
    }
    const bool stored = insert.bind(2, upload.user)
                            .bind(3, upload.path.raw)
                            .bind(4, static_cast<qint64>(upload.size))
                            .bind(5, static_cast<qint64>(info.sent))
                            .bind(6, upload.startedMs)
                            .bind(7, m_now())
                            .bind(8, outcomeOf(info.state))
                            .run();
    if (!stored) {
        qCWarning(lcShares) << "couldn't record an upload:" << m_db.lastError();
    }
}

void ShareService::refreshTotals()
{
    const qint64 monthAgo = m_now() - (30 * kDayMs);
    m_status.uploadedAllTime = uploadTotals(m_db, 0);
    m_status.uploaded30Days = uploadTotals(m_db, monthAgo);
    m_status.downloadedAllTime = downloadTotals(m_db, 0);
    m_status.downloaded30Days = downloadTotals(m_db, monthAgo);
    if (m_session) {
        m_status.uploadedSession = uploadTotals(m_db, 0, m_session);
        m_status.downloadedSession = downloadTotals(m_db, 0, m_session);
    }
    publish();
}

QList<UploadEntry> ShareService::uploads() const
{
    QList<UploadEntry> entries;
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it) {
        const Active &active = *it.value();
        entries.append(UploadEntry{.transfer = it.key(),
                                   .user = active.user,
                                   .path = active.path.display(),
                                   .size = active.size,
                                   .sent = active.sent,
                                   .active = true});
    }
    return entries;
}

ShareStatus ShareService::status() const
{
    ShareStatus status = m_status;
    status.uploading = m_queue.active();
    status.queued = m_queue.queued();
    return status;
}

void ShareService::publish()
{
    Q_EMIT statusChanged(status());
}

void ShareService::publishUploads()
{
    Q_EMIT uploadsChanged(uploads());
}

void ShareService::waitForIdle()
{
    for (int round = 0; round < 100; ++round) {
        m_pool.waitForDone();
        bool pending = false;
        {
            const std::scoped_lock lock(m_mutex);
            pending = !m_built.isEmpty();
        }
        if (!pending && !m_building) {
            return;
        }
        drain();
    }
}

} // namespace selecta::core
