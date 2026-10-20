// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TransferService.h"

#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "library/Naming.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcTransfers, "selecta.core.transfers")

using State = Download::State;

// Waits before each retry of a transient failure (Limits.h: kDownloadRetries).
constexpr std::array<std::chrono::minutes, 3> kRetryDelays{3min, 10min, 30min};
static_assert(kRetryDelays.size() == governor::limits::kDownloadRetries);
const QString kPartialSuffix = u".part"_s;

QList<qint64> defaultRetryDelays()
{
    QList<qint64> delays;
    for (const std::chrono::minutes delay : kRetryDelays) {
        delays.append(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
    }
    return delays;
}

bool transient(slsk::TransferError error)
{
    switch (error) {
    case slsk::TransferError::ConnectionFailed:
    case slsk::TransferError::RemoteFailed:
    case slsk::TransferError::ConnectionLost:
    case slsk::TransferError::Timeout:
        return true;
    case slsk::TransferError::None:
    case slsk::TransferError::Denied:
    case slsk::TransferError::LocalError:
    case slsk::TransferError::ProtocolError:
        break;
    }
    return false;
}

// Why a transfer failed, in words.
QString failureText(const slsk::DownloadInfo &info)
{
    switch (info.error) {
    case slsk::TransferError::ConnectionFailed:
        return TransferService::tr("%1 couldn't be reached").arg(info.request.username);
    case slsk::TransferError::Denied:
        return TransferService::tr("%1 refused: %2")
            .arg(info.request.username, info.rejection ? info.rejection->text : TransferService::tr("no reason"));
    case slsk::TransferError::RemoteFailed:
        return TransferService::tr("%1's side of the transfer failed").arg(info.request.username);
    case slsk::TransferError::ConnectionLost:
        return TransferService::tr("the connection to %1 dropped").arg(info.request.username);
    case slsk::TransferError::Timeout:
        return TransferService::tr("%1 stopped answering").arg(info.request.username);
    case slsk::TransferError::LocalError:
        return TransferService::tr("the file couldn't be written here");
    case slsk::TransferError::ProtocolError:
        return TransferService::tr("%1's client broke the protocol").arg(info.request.username);
    case slsk::TransferError::None:
        break;
    }
    return info.detail.isEmpty() ? TransferService::tr("the transfer failed") : info.detail;
}

QString minutesText(qint64 ms)
{
    const qint64 minutes = std::max<qint64>(1, (ms + 59'999) / 60'000);
    return minutes == 1 ? TransferService::tr("1 minute") : TransferService::tr("%1 minutes").arg(minutes);
}

} // namespace

// Partial files in the incoming folder, for slsk's TransferManager.
class TransferService::Storage final : public slsk::DownloadStorage
{
public:
    explicit Storage(TransferService &service)
        : m_service(service)
    {}
    QIODevice *openDownload(const slsk::DownloadInfo &download) override { return m_service.openFile(download); }
    void closeDownload(const slsk::DownloadInfo &download, QIODevice *device) override
    {
        m_service.closeFile(download, device);
    }

private:
    TransferService &m_service;
};

TransferService::TransferService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                                 ActivityLog &activity, Options options, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_activity(activity)
    , m_options(std::move(options))
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_store(db)
    , m_storage(std::make_unique<Storage>(*this))
    , m_retryTimer(new QTimer(this))
{
    qRegisterMetaType<Download>();
    if (m_options.retryDelaysMs.isEmpty()) {
        m_options.retryDelaysMs = defaultRetryDelays();
    }
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &TransferService::startWaiting);
    slsk::TransferManager &transfers = m_network.transfers();
    transfers.setDownloadStorage(m_storage.get());
    connect(&transfers, &slsk::TransferManager::downloadChanged, this, &TransferService::transferChanged);
    connect(&transfers, &slsk::TransferManager::downloadProgress, this, &TransferService::transferProgress);
    connect(&m_network, &NetworkService::statusChanged, this, &TransferService::networkChanged);
    connect(&m_governor, &governor::Governor::pausedChanged, this, [this](bool paused) {
        if (!paused) {
            startWaiting();
        }
    });
}

TransferService::~TransferService()
{
    if (!m_shutDown) {
        // Destroyed before the network (tests): stop the transfers writing into our files, then let go of them.
        disconnect(&m_network.transfers(), nullptr, this, nullptr);
        const QList<slsk::TransferId> transfers = m_downloadOf.keys();
        for (const slsk::TransferId id : transfers) {
            m_network.transfers().cancelDownload(id); // closes its file through Storage
        }
        m_network.transfers().setDownloadStorage(nullptr);
    }
    qDeleteAll(m_files);
}

void TransferService::shutdown()
{
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    m_retryTimer->stop();
    disconnect(&m_network.transfers(), nullptr, this, nullptr);
    disconnect(&m_network, nullptr, this, nullptr);
    disconnect(&m_governor, nullptr, this, nullptr);
    // The TransferManager keeps using Storage until it is destroyed, closing the open files then.
}

void TransferService::load()
{
    if (m_shutDown) {
        return;
    }
    QList<Download> converting;
    const QList<Download> saved = m_store.unfinished();
    for (Download download : saved) {
        switch (download.state) {
        case State::Queued:
        case State::Transferring:
            // The peer may still have us queued, but this launch has no transfer for it: ask again.
            download.state = State::Waiting;
            download.message = tr("Resuming");
            download.retryAtMs.reset();
            break;
        case State::Converting:
            converting.append(download);
            break;
        case State::Waiting:
        case State::Arrived:
        case State::Failed:
        case State::Cancelled:
            break;
        }
        if (download.state == State::Waiting && download.retryAtMs) {
            // Saved as a wall-clock time; timed from now on with the monotonic clock.
            m_retryAt.insert(download.id, QDeadlineTimer(std::max<qint64>(0, *download.retryAtMs - m_now())));
        }
        // What is on disk is what a resumed transfer continues from.
        if (download.state != State::Converting) {
            const QFileInfo partial(partialPathOf(download));
            download.received = partial.exists() ? partial.size() : 0;
        }
        m_live.insert(download.id, download);
        save(m_live[download.id]);
    }
    for (const Download &download : std::as_const(converting)) {
        if (QFileInfo::exists(download.localPath)) {
            Q_EMIT completed(download);
        } else {
            Download &live = m_live[download.id];
            finish(live, State::Failed, tr("The downloaded file is gone; download it again."));
        }
    }
    startWaiting();
}

std::optional<qint64> TransferService::enqueue(const DownloadSpec &spec)
{
    if (m_shutDown || spec.peer.isEmpty() || spec.remotePath.isEmpty()) {
        return std::nullopt;
    }
    for (const Download &live : std::as_const(m_live)) {
        if (live.peer == spec.peer && live.remotePath == spec.remotePath) {
            return live.id; // already wanted
        }
    }
    Download download;
    download.peer = spec.peer;
    download.remotePath = spec.remotePath;
    download.size = std::max<qint64>(0, spec.size);
    download.file = spec.file;
    download.release = spec.release;
    download.getLine = spec.getLine;
    download.state = State::Waiting;
    download.createdAt = m_now();
    download.updatedAt = download.createdAt;
    const std::optional<qint64> id = m_store.add(download);
    if (!id) {
        qCWarning(lcTransfers) << "couldn't save a download:" << m_db.lastError();
        return std::nullopt;
    }
    download.id = *id;
    m_live.insert(download.id, download);
    m_activity.record(u"download"_s, download.peer, u"wanted"_s, download.remoteFileName());
    Q_EMIT changed(download);
    tryStart(m_live[download.id]);
    return id;
}

void TransferService::cancel(qint64 id)
{
    if (m_shutDown) {
        return;
    }
    const auto found = m_live.find(id);
    if (found == m_live.end()) {
        return;
    }
    if (const auto transfer = m_transferOf.constFind(id); transfer != m_transferOf.cend()) {
        // The manager reports the cancellation through transferChanged(), which finishes the download.
        m_network.transfers().cancelDownload(transfer.value());
        if (!m_live.contains(id)) {
            return; // already finished by that report
        }
    }
    Download &download = m_live[id];
    // Converting: the arrival pipeline is told by the engine (Cancelled); its file goes with the folder.
    finish(download, State::Cancelled, tr("Cancelled"));
}

void TransferService::retry(qint64 id)
{
    if (m_shutDown) {
        return;
    }
    std::optional<Download> download = m_store.find(id);
    if (!download || download->state != State::Failed) {
        return;
    }
    if (m_store.findUnfinished(download->peer, download->remotePath)) {
        return; // the same file is being downloaded again already
    }
    download->state = State::Waiting;
    download->attempts = 0;
    download->retryAtMs.reset();
    m_retryAt.remove(id);
    download->message.clear();
    m_live.insert(id, *download);
    save(m_live[id]);
    tryStart(m_live[id]);
}

void TransferService::arrived(qint64 id, qint64 inboxItem)
{
    const auto found = m_live.find(id);
    if (m_shutDown || found == m_live.end() || found->state != State::Converting) {
        return;
    }
    found->inboxItem = inboxItem;
    finish(*found, State::Arrived, tr("In your Inbox"));
}

void TransferService::arrivalFailed(qint64 id, const QString &message)
{
    const auto found = m_live.find(id);
    if (m_shutDown || found == m_live.end() || found->state != State::Converting) {
        return;
    }
    finish(*found, State::Failed, message);
}

std::optional<Download> TransferService::download(qint64 id)
{
    if (const auto found = m_live.constFind(id); found != m_live.cend()) {
        return found.value();
    }
    return m_store.find(id);
}

QList<Download> TransferService::unfinished() const
{
    QList<Download> downloads = m_live.values();
    std::ranges::sort(downloads, [](const Download &a, const Download &b) {
        return a.createdAt < b.createdAt || (a.createdAt == b.createdAt && a.id < b.id);
    });
    return downloads;
}

void TransferService::networkChanged(const NetworkStatus &status)
{
    if (status.state == NetworkStatus::State::Online) {
        startWaiting();
    }
}

void TransferService::startWaiting()
{
    if (m_shutDown) {
        return;
    }
    // Oldest first: what was asked for first goes first.
    QList<qint64> waiting;
    for (const Download &download : std::as_const(m_live)) {
        if (download.state == State::Waiting && isDue(download.id)) {
            waiting.append(download.id);
        }
    }
    std::ranges::sort(waiting);
    for (const qint64 id : std::as_const(waiting)) {
        if (const auto found = m_live.find(id); found != m_live.end() && found->state == State::Waiting) {
            tryStart(*found);
        }
    }
    scheduleRetries();
}

void TransferService::tryStart(Download &download)
{
    if (download.state != State::Waiting || !isDue(download.id)) {
        return;
    }
    if (!m_network.isOnline()) {
        if (download.message != tr("Waiting for the Soulseek connection")) {
            download.message = tr("Waiting for the Soulseek connection");
            save(download);
        }
        return;
    }
    governor::Request request;
    request.peer = download.peer;
    governor::Decision decision = m_governor.request(governor::Operation::QueueDownload, request);
    if (!decision.permit) {
        switch (decision.refusal) {
        case governor::Refusal::PeerDailyLimit:
        case governor::Refusal::PeerWeeklyLimit:
            // Not today: Get tracks may pick another peer's copy.
            finish(download, State::Failed, decision.explanation);
            return;
        case governor::Refusal::NetworkPaused:
            // Starts again as soon as the user resumes (pausedChanged).
            download.retryAtMs.reset();
            m_retryAt.remove(download.id);
            download.message = decision.explanation;
            save(download);
            return;
        default:
            break;
        }
        const qint64 delay = decision.retryAfter.count() > 0
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(decision.retryAfter).count()
                                 : m_options.pacedRetryMs;
        wait(download, delay, decision.explanation);
        return;
    }
    slsk::DownloadRequest transfer;
    transfer.username = download.peer;
    transfer.path = slsk::RemotePath(download.remotePath);
    transfer.expectedSize = static_cast<quint64>(std::max<qint64>(0, download.size));
    const slsk::TransferId id = m_network.transfers().download(transfer, std::move(*decision.permit));
    if (id == 0) {
        finish(download, State::Failed, tr("Selecta couldn't ask %1 for this file.").arg(download.peer));
        return;
    }
    m_transferOf.insert(download.id, id);
    m_downloadOf.insert(id, download.id);
    m_governor.downloadQueued(download.peer);
    download.state = State::Queued;
    download.retryAtMs.reset();
    m_retryAt.remove(download.id);
    download.message = tr("Queued at %1").arg(download.peer);
    save(download);
}

void TransferService::transferChanged(const slsk::DownloadInfo &info)
{
    const qint64 id = m_downloadOf.value(info.id);
    const auto found = m_live.find(id);
    if (id == 0 || found == m_live.end()) {
        return;
    }
    Download &download = *found;
    switch (info.state) {
    case slsk::DownloadState::Queued:
        download.message = info.placeInQueue ? tr("Queued at %1, place %2").arg(download.peer).arg(*info.placeInQueue)
                                             : tr("Queued at %1").arg(download.peer);
        save(download);
        break;
    case slsk::DownloadState::Requested:
    case slsk::DownloadState::Connecting:
        if (!m_active.contains(id)) {
            m_active.insert(id);
            m_governor.downloadStarted(download.peer);
        }
        download.message = tr("Connecting to %1").arg(download.peer);
        save(download);
        break;
    case slsk::DownloadState::Transferring:
        if (!m_active.contains(id)) {
            m_active.insert(id);
            m_governor.downloadStarted(download.peer);
        }
        if (download.state != State::Transferring) {
            download.state = State::Transferring;
            download.message = tr("Downloading from %1").arg(download.peer);
            if (info.size > 0) {
                download.size = static_cast<qint64>(info.size);
            }
            save(download);
        }
        break;
    case slsk::DownloadState::Completed: {
        const auto bytes = static_cast<qint64>(info.startOffset + info.received);
        m_governor.downloadFinished(download.peer, bytes, true);
        forget(id);
        download.received = bytes;
        download.state = State::Converting;
        download.message = tr("Converting");
        m_activity.record(u"download"_s, download.peer, u"completed"_s, download.remoteFileName());
        save(download);
        Q_EMIT completed(download);
        break;
    }
    case slsk::DownloadState::Failed:
        failed(download, info);
        break;
    case slsk::DownloadState::Cancelled:
        release(download, static_cast<qint64>(info.startOffset + info.received));
        finish(download, State::Cancelled, tr("Cancelled"));
        break;
    }
}

void TransferService::failed(Download &download, const slsk::DownloadInfo &info)
{
    release(download, static_cast<qint64>(info.startOffset + info.received));
    const QString reason = failureText(info);
    m_activity.record(u"download"_s, download.peer, u"failed"_s, download.remoteFileName() + u": "_s + reason);

    bool retryable = transient(info.error);
    if (info.error == slsk::TransferError::Denied && info.rejection) {
        m_governor.peerDenied(download.peer, info.rejection->text);
        retryable = info.rejection->rejectionClass == slsk::RejectionClass::Retryable;
    }
    if (retryable && download.attempts < m_options.retryDelaysMs.size()) {
        const qint64 delay = m_options.retryDelaysMs.at(download.attempts);
        ++download.attempts;
        wait(download, delay, tr("%1; trying again in %2").arg(reason, minutesText(delay)));
        return;
    }
    finish(download, State::Failed, reason);
}

void TransferService::transferProgress(slsk::TransferId id, quint64 done, quint64 total)
{
    const qint64 downloadId = m_downloadOf.value(id);
    const auto found = m_live.find(downloadId);
    if (downloadId == 0 || found == m_live.end()) {
        return;
    }
    found->received = static_cast<qint64>(done);
    if (total > 0) {
        found->size = static_cast<qint64>(total);
    }
    Q_EMIT progress(downloadId, found->received, found->size);
}

void TransferService::wait(Download &download, qint64 delayMs, const QString &message)
{
    download.state = State::Waiting;
    download.retryAtMs = m_now() + std::max<qint64>(0, delayMs); // shown and saved
    m_retryAt.insert(download.id, QDeadlineTimer(std::max<qint64>(0, delayMs)));
    download.message = message;
    save(download);
    scheduleRetries();
}

void TransferService::finish(Download &download, State state, const QString &message)
{
    const qint64 id = download.id;
    if (const auto transfer = m_transferOf.constFind(id); transfer != m_transferOf.cend()) {
        // Still known to the manager (its report is queued behind the one being handled): settle the governor's books
        // now and stop the transfer; the late report finds nothing to act on.
        const slsk::TransferId transferId = transfer.value();
        release(download, download.received);
        m_network.transfers().cancelDownload(transferId);
    }
    download.state = state;
    download.message = message;
    download.retryAtMs.reset();
    m_retryAt.remove(id);
    if (state == State::Cancelled) {
        deleteFiles(download);
        download.localPath.clear();
    } else if (state == State::Arrived) {
        download.localPath.clear(); // the pipeline trashed or deleted the original, and kept it if it couldn't
    }
    save(download);
    if (state == State::Failed) {
        qCInfo(lcTransfers).noquote() << "download" << id << "failed:" << message;
    }
    m_live.remove(id); // `download` refers to the removed entry from here on
}

void TransferService::release(Download &download, qint64 bytes)
{
    const qint64 id = download.id;
    if (!m_transferOf.contains(id)) {
        return;
    }
    if (m_active.contains(id)) {
        m_governor.downloadFinished(download.peer, bytes, false);
    } else {
        m_governor.downloadWithdrawn(download.peer);
    }
    forget(id);
}

void TransferService::forget(qint64 id)
{
    const slsk::TransferId transfer = m_transferOf.take(id);
    m_downloadOf.remove(transfer);
    m_active.remove(id);
}

void TransferService::save(Download &download)
{
    download.updatedAt = m_now();
    if (!m_store.update(download)) {
        qCWarning(lcTransfers) << "couldn't save download" << download.id << ':' << m_db.lastError();
    }
    Q_EMIT changed(download);
}

bool TransferService::isDue(qint64 id) const
{
    const auto found = m_retryAt.constFind(id);
    return found == m_retryAt.cend() || found->hasExpired();
}

void TransferService::scheduleRetries()
{
    std::optional<qint64> next;
    for (auto it = m_retryAt.cbegin(); it != m_retryAt.cend(); ++it) {
        const auto found = m_live.constFind(it.key());
        if (found != m_live.cend() && found->state == State::Waiting) {
            const qint64 remaining = it->remainingTime();
            next = next ? std::min(*next, remaining) : remaining;
        }
    }
    if (!next) {
        m_retryTimer->stop();
        return;
    }
    m_retryTimer->start(static_cast<int>(std::clamp<qint64>(*next, 0, std::numeric_limits<int>::max())));
}

void TransferService::deleteFiles(const Download &download)
{
    const QString folder = folderOf(download.id);
    if (!m_options.incomingDir.isEmpty() && QFileInfo::exists(folder) && !QDir(folder).removeRecursively()) {
        qCWarning(lcTransfers) << "couldn't remove" << folder;
    }
}

QString TransferService::folderOf(qint64 id) const
{
    return QDir(m_options.incomingDir).filePath(QString::number(id));
}

QString TransferService::partialPathOf(const Download &download) const
{
    QString name = library::naming::safeComponent(download.remoteFileName());
    if (name.isEmpty()) {
        name = u"download"_s;
    }
    return QDir(folderOf(download.id)).filePath(name + kPartialSuffix);
}

QIODevice *TransferService::openFile(const slsk::DownloadInfo &info)
{
    const qint64 id = m_downloadOf.value(info.id);
    const auto found = m_live.find(id);
    if (m_shutDown || id == 0 || found == m_live.end() || m_options.incomingDir.isEmpty()) {
        return nullptr; // not ours to take: the manager declines it
    }
    const QString path = partialPathOf(*found);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qCWarning(lcTransfers) << "can't create" << QFileInfo(path).absolutePath();
        return nullptr;
    }
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        qCWarning(lcTransfers) << "can't open" << path << ':' << file->errorString();
        return nullptr;
    }
    found->localPath = path;
    QFile *device = file.release();
    m_files.insert(info.id, device);
    return device;
}

void TransferService::closeFile(const slsk::DownloadInfo &info, QIODevice *device)
{
    QFile *file = m_files.take(info.id);
    if (file == nullptr || file != device) {
        qCWarning(lcTransfers) << "asked to close a file Selecta didn't open, transfer" << info.id;
        return;
    }
    const QString partial = file->fileName();
    file->flush();
    file->close();
    delete file;
    if (info.state != slsk::DownloadState::Completed) {
        return; // kept for a resume, or deleted when the download is cancelled
    }
    QString complete = partial;
    complete.chop(kPartialSuffix.size());
    QFile::remove(complete);
    const qint64 id = m_downloadOf.value(info.id);
    if (!QFile::rename(partial, complete)) {
        qCWarning(lcTransfers) << "couldn't rename" << partial;
        complete = partial; // still a complete file; the arrival pipeline reads it as it is
    }
    if (const auto found = m_live.find(id); found != m_live.end()) {
        found->localPath = std::move(complete);
    }
}

} // namespace selecta::core
