// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/TransferManager.h"

#include "slsk/Frame.h"
#include "slsk/Logging.h"
#include "slsk/MetaTypes.h"
#include "slsk/PeerMessages.h"
#include "slsk/ServerSession.h"
#include "slsk/TokenGenerator.h"

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QIODevice>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::slsk {

DownloadStorage::~DownloadStorage() = default;

bool DownloadInfo::isTerminal() const
{
    return state == DownloadState::Completed || state == DownloadState::Failed || state == DownloadState::Cancelled;
}

bool UploadInfo::isTerminal() const
{
    return state == UploadState::Completed || state == UploadState::Failed || state == UploadState::Cancelled ||
           state == UploadState::Declined;
}

const char *downloadStateName(DownloadState state)
{
    switch (state) {
    case DownloadState::Queued:
        return "queued";
    case DownloadState::Requested:
        return "requested";
    case DownloadState::Connecting:
        return "connecting";
    case DownloadState::Transferring:
        return "transferring";
    case DownloadState::Completed:
        return "completed";
    case DownloadState::Failed:
        return "failed";
    case DownloadState::Cancelled:
        return "cancelled";
    }
    return "?";
}

const char *uploadStateName(UploadState state)
{
    switch (state) {
    case UploadState::Requested:
        return "requested";
    case UploadState::Connecting:
        return "connecting";
    case UploadState::Transferring:
        return "transferring";
    case UploadState::Completed:
        return "completed";
    case UploadState::Failed:
        return "failed";
    case UploadState::Cancelled:
        return "cancelled";
    case UploadState::Declined:
        return "declined";
    }
    return "?";
}

const char *transferErrorName(TransferError error)
{
    switch (error) {
    case TransferError::None:
        return "none";
    case TransferError::ConnectionFailed:
        return "connection failed";
    case TransferError::Denied:
        return "denied";
    case TransferError::RemoteFailed:
        return "the uploader reported a failure";
    case TransferError::ConnectionLost:
        return "connection lost";
    case TransferError::Timeout:
        return "timed out";
    case TransferError::LocalError:
        return "local error";
    case TransferError::ProtocolError:
        return "protocol error";
    }
    return "?";
}

namespace internal {
namespace {

constexpr qint64 kChunk = qint64{64} * 1024;           // bytes read from an upload source at a time
constexpr qint64 kWriteWatermark = qint64{256} * 1024; // unsent bytes we keep queued on an upload socket
constexpr std::chrono::milliseconds kBudgetTick = 100ms;
constexpr std::chrono::milliseconds kFlushTimeout = 5s; // a completed download's last write, before we cut it off

struct Download
{
    DownloadInfo info;
    quint32 token = 0; // the uploader's TransferRequest token
    QIODevice *device = nullptr;
    QTcpSocket *socket = nullptr;
    QDeadlineTimer deadline = QDeadlineTimer::Forever;
    bool progressDirty = false;
};

struct Upload
{
    UploadInfo info;
    quint32 token = 0; // our TransferRequest token
    QIODevice *source = nullptr;
    quint64 fileRequest = 0;
    QTcpSocket *socket = nullptr;
    bool allowed = false;
    bool offsetReceived = false;
    bool allQueued = false; // every byte handed to the socket; waiting for the downloader to close
    QDeadlineTimer deadline = QDeadlineTimer::Forever;
    QElapsedTimer clock; // for the average speed
    bool progressDirty = false;
};

// What an F socket we own is for.
struct FileLink
{
    enum class Kind {
        Unassigned, // an inbound socket from `username`, waiting for the token that tells which download it is
        Download,
        Upload,
    };
    Kind kind = Kind::Unassigned;
    QString username;
    TransferId id = 0;
    QByteArray buffer;                                 // handshake bytes not yet complete
    QDeadlineTimer deadline = QDeadlineTimer::Forever; // an unassigned socket's wait for its token
};

QString timeoutDetail(DownloadState state)
{
    if (state == DownloadState::Requested) {
        return u"the uploader never connected"_s;
    }
    return state == DownloadState::Connecting ? u"the uploader sent no data"_s : u"the transfer stalled"_s;
}

QString timeoutDetail(const Upload &u)
{
    if (u.info.state == UploadState::Transferring) {
        return u"the downloader stopped reading"_s;
    }
    return u.allowed ? u"the transfer didn't start in time"_s : u"the downloader never answered"_s;
}

// A progress signal due, collected before any is emitted (a slot may start or end transfers).
struct ProgressReport
{
    bool upload = false;
    TransferId id = 0;
    quint64 done = 0;
    quint64 total = 0;
};

} // namespace

class TransferManagerPrivate
{
public:
    TransferManagerPrivate(TransferManager *owner, PeerNetwork &network, ServerSession &session)
        : q(owner)
        , peers(network)
        , server(session)
        , sweep(new QTimer(owner))
        , budgetTimer(new QTimer(owner))
        , progressTimer(new QTimer(owner))
    {}
    ~TransferManagerPrivate() = default;
    TransferManagerPrivate(const TransferManagerPrivate &) = delete;
    TransferManagerPrivate &operator=(const TransferManagerPrivate &) = delete;
    TransferManagerPrivate(TransferManagerPrivate &&) = delete;
    TransferManagerPrivate &operator=(TransferManagerPrivate &&) = delete;

    // --- downloads ---------------------------------------------------------------------------------------------------
    Download *findDownload(const QString &username, const RemotePath &path);
    void onTransferRequest(const QString &username, const peer::TransferRequest &request);
    void onUploadFailed(const QString &username, const peer::UploadFailed &message);
    void onUploadDenied(const QString &username, const peer::UploadDenied &message);
    void onPlaceInQueue(const QString &username, const peer::PlaceInQueueResponse &response);
    void setDownloadState(Download &d, DownloadState state);
    void finishDownload(TransferId id, DownloadState state, TransferError error, const QString &detail);
    void downloadHandshake(QTcpSocket *socket, FileLink &link);
    void receive(QTcpSocket *socket, TransferId id);
    void markProgress(Download &d) const;
    void emitProgress(const Download &d);

    // --- uploads -----------------------------------------------------------------------------------------------------
    void onTransferResponse(const QString &username, const peer::TransferResponse &response);
    void setUploadState(Upload &u, UploadState state);
    void finishUpload(TransferId id, UploadState state, TransferError error, const QString &detail,
                      bool tellDownloader);
    void uploadHandshake(QTcpSocket *socket, FileLink &link);
    void pump(TransferId id);
    void pumpAll();
    void markProgress(Upload &u) const;
    void emitProgress(const Upload &u);
    void flushProgress();

    // --- signals -----------------------------------------------------------------------------------------------------
    // Every signal goes through here. One caused inside a slot (a receiver cancelling a transfer, say) is delivered
    // once the current one has reached every receiver, so all receivers see the changes in the order they happened.
    void notify(std::function<void()> emitter);
    void notifyDownload(const DownloadInfo &info);
    void notifyUpload(const UploadInfo &info);

    // --- F sockets ---------------------------------------------------------------------------------------------------
    bool expectsFileConnection(const QString &username) const;
    void onFileConnection(quint64 requestId, const QString &username, QTcpSocket *socket, const QByteArray &pending);
    void onFileConnectionFailed(quint64 requestId, PeerError error);
    void adopt(QTcpSocket *socket, FileLink link);
    void onSocketData(QTcpSocket *socket);
    void onSocketClosed(QTcpSocket *socket);
    void dropSocket(QTcpSocket *socket, bool flush = false);
    void onSweep();

    TransferManager *q;
    PeerNetwork &peers;
    ServerSession &server;
    DownloadStorage *storage = nullptr;
    TransferManager::Timeouts timeouts;
    QTimer *sweep;
    QTimer *budgetTimer;
    QTimer *progressTimer;
    qint64 speedLimit = 0; // bytes per second; 0 = none
    qint64 budget = 0;     // bytes uploads may still send this tick
    std::map<TransferId, Download> downloads;
    std::map<TransferId, Upload> uploads;
    std::map<QTcpSocket *, FileLink> links;
    TokenGenerator transferTokens;
    TransferId nextId = 1;
    QList<std::function<void()>> pendingSignals;
    bool notifying = false;
};

// --- signals ---------------------------------------------------------------------------------------------------------

void TransferManagerPrivate::notify(std::function<void()> emitter)
{
    pendingSignals.append(std::move(emitter));
    if (notifying) {
        return; // delivered by the call below us on the stack, after the signal it's in the middle of
    }
    notifying = true;
    const QPointer<TransferManager> guard(q);
    while (!pendingSignals.isEmpty()) {
        const std::function<void()> next = pendingSignals.takeFirst();
        next();
        if (!guard) {
            return; // a receiver destroyed the manager
        }
    }
    notifying = false;
}

void TransferManagerPrivate::notifyDownload(const DownloadInfo &info)
{
    notify([this, info] { Q_EMIT q->downloadChanged(info); });
}

void TransferManagerPrivate::notifyUpload(const UploadInfo &info)
{
    notify([this, info] { Q_EMIT q->uploadChanged(info); });
}

// --- downloads -------------------------------------------------------------------------------------------------------

Download *TransferManagerPrivate::findDownload(const QString &username, const RemotePath &path)
{
    for (auto &[id, d] : downloads) {
        if (d.info.request.username == username && d.info.request.path == path) {
            return &d;
        }
    }
    return nullptr;
}

void TransferManagerPrivate::setDownloadState(Download &d, DownloadState state)
{
    d.info.state = state;
    notifyDownload(d.info);
}

void TransferManagerPrivate::markProgress(Download &d) const
{
    d.progressDirty = true;
    if (!progressTimer->isActive()) {
        progressTimer->start(timeouts.progress);
    }
}

void TransferManagerPrivate::emitProgress(const Download &d)
{
    notify([this, id = d.info.id, done = d.info.startOffset + d.info.received, total = d.info.size] {
        Q_EMIT q->downloadProgress(id, done, total);
    });
}

void TransferManagerPrivate::markProgress(Upload &u) const
{
    u.progressDirty = true;
    if (!progressTimer->isActive()) {
        progressTimer->start(timeouts.progress);
    }
}

void TransferManagerPrivate::emitProgress(const Upload &u)
{
    notify([this, id = u.info.id, done = u.info.startOffset + u.info.sent, total = u.info.request.size] {
        Q_EMIT q->uploadProgress(id, done, total);
    });
}

void TransferManagerPrivate::flushProgress()
{
    // One signal per transfer per tick, with the latest numbers; the timer stops when nothing moved.
    QList<ProgressReport> due;
    for (auto &[id, d] : downloads) {
        if (std::exchange(d.progressDirty, false)) {
            due.append({false, id, d.info.startOffset + d.info.received, d.info.size});
        }
    }
    for (auto &[id, u] : uploads) {
        if (std::exchange(u.progressDirty, false)) {
            due.append({true, id, u.info.startOffset + u.info.sent, u.info.request.size});
        }
    }
    if (due.isEmpty()) {
        progressTimer->stop();
        return;
    }
    for (const ProgressReport &report : std::as_const(due)) {
        notify([this, report] {
            // Never after a transfer's terminal signal, which a receiver of an earlier one may have caused.
            if (report.upload && uploads.contains(report.id)) {
                Q_EMIT q->uploadProgress(report.id, report.done, report.total);
            } else if (!report.upload && downloads.contains(report.id)) {
                Q_EMIT q->downloadProgress(report.id, report.done, report.total);
            }
        });
    }
}

void TransferManagerPrivate::finishDownload(TransferId id, DownloadState state, TransferError error,
                                            const QString &detail)
{
    const auto it = downloads.find(id);
    if (it == downloads.end()) {
        return;
    }
    Download d = std::move(it->second);
    downloads.erase(it);
    if (d.socket != nullptr) {
        // Completed: we close, gracefully (protocol §7). Otherwise: at once.
        dropSocket(d.socket, state == DownloadState::Completed);
    }
    d.info.state = state;
    d.info.error = error;
    if (!detail.isEmpty()) {
        d.info.detail = detail;
    }
    if (d.device != nullptr && storage != nullptr) {
        storage->closeDownload(d.info, d.device);
    }
    if (state == DownloadState::Completed) {
        emitProgress(d); // the final numbers, at once
        qCInfo(lcSlskTransfer).noquote() << "download complete:" << d.info.request.path.display() << "from"
                                         << d.info.request.username;
    } else {
        qCInfo(lcSlskTransfer).noquote() << "download" << downloadStateName(state) << ":"
                                         << d.info.request.path.display() << transferErrorName(error) << detail;
    }
    notifyDownload(d.info);
}

void TransferManagerPrivate::onTransferRequest(const QString &username, const peer::TransferRequest &request)
{
    if (request.direction == TransferDirection::Download) {
        // A legacy download request: never accepted directly (the peer could be spoofed); queued instead and
        // answered "Queued", as every client does (protocol §6.4).
        peers.sendPeerMessage(
            username,
            frame(peer::TransferResponse{request.token, false, std::nullopt, rejectionText(RejectionReason::Queued)}),
            ConnectPurpose::Transfer);
        notify([this, username, path = request.path] { Q_EMIT q->uploadRequested(username, path, true); });
        return;
    }
    if (request.direction != TransferDirection::Upload) {
        qCDebug(lcSlskTransfer) << "ignored a TransferRequest with direction"
                                << static_cast<quint32>(request.direction);
        return;
    }
    const auto decline = [&](const QString &why) {
        qCDebug(lcSlskTransfer).noquote() << "declined a transfer from" << username << ":" << why;
        peers.sendPeerMessage(username,
                              frame(peer::TransferResponse{request.token, false, std::nullopt,
                                                           rejectionText(RejectionReason::Cancelled)}),
                              ConnectPurpose::Transfer);
    };
    Download *d = findDownload(username, request.path);
    // Nothing queued (unsolicited, or a queued download we withdrew): "Cancelled" frees the uploader's slot at once.
    if (d == nullptr) {
        decline(u"not wanted"_s);
        return;
    }
    if (d->info.state == DownloadState::Transferring) {
        decline(u"already transferring"_s);
        return;
    }
    if (d->device == nullptr) {
        d->device = storage != nullptr ? storage->openDownload(d->info) : nullptr;
        if (d->device == nullptr) {
            decline(u"no storage"_s);
            finishDownload(d->info.id, DownloadState::Failed, TransferError::LocalError,
                           u"the download's file couldn't be opened"_s);
            return;
        }
    }
    // The uploader's size unless it says 0 (SoulseekQt over 2 GiB): then the search result's.
    const quint64 size = request.size.value_or(0);
    d->info.size = size > 0 ? size : d->info.request.expectedSize;
    d->info.placeInQueue.reset();
    d->token = request.token;
    if (d->socket != nullptr) {
        dropSocket(d->socket); // a retry replaces a handshake that never finished
        d->socket = nullptr;
    }
    d->deadline.setRemainingTime(timeouts.requested);
    peers.sendPeerMessage(username, frame(peer::TransferResponse{request.token, true, std::nullopt, {}}),
                          ConnectPurpose::Transfer);
    setDownloadState(*d, DownloadState::Requested);
}

void TransferManagerPrivate::onUploadFailed(const QString &username, const peer::UploadFailed &message)
{
    const Download *d = findDownload(username, message.path);
    // Only an active transfer can fail; for a queued download (or one we cancelled, which is gone) it's the echo of
    // an earlier transfer and is ignored.
    if (d == nullptr || d->info.state == DownloadState::Queued) {
        qCDebug(lcSlskTransfer).noquote() << "ignored UploadFailed for" << message.path.display();
        return;
    }
    finishDownload(d->info.id, DownloadState::Failed, TransferError::RemoteFailed,
                   u"the uploader reported a failure"_s);
}

void TransferManagerPrivate::onUploadDenied(const QString &username, const peer::UploadDenied &message)
{
    Download *d = findDownload(username, message.path);
    if (d == nullptr) {
        return;
    }
    d->info.rejection = classifyRejection(message.reason);
    finishDownload(d->info.id, DownloadState::Failed, TransferError::Denied, message.reason);
}

void TransferManagerPrivate::onPlaceInQueue(const QString &username, const peer::PlaceInQueueResponse &response)
{
    Download *d = findDownload(username, response.path);
    if (d == nullptr || d->info.state != DownloadState::Queued) {
        return;
    }
    d->info.placeInQueue = response.place;
    notifyDownload(d->info);
}

void TransferManagerPrivate::downloadHandshake(QTcpSocket *socket, FileLink &link)
{
    if (link.buffer.size() < file::kTransferInitBytes) {
        return;
    }
    const quint32 token = file::decodeTransferInit(link.buffer.first(file::kTransferInitBytes)).value_or(0);
    link.buffer.remove(0, file::kTransferInitBytes);
    Download *d = nullptr;
    for (auto &[id, candidate] : downloads) {
        if (candidate.info.request.username == link.username && candidate.token == token &&
            candidate.info.state == DownloadState::Requested) {
            d = &candidate;
            break;
        }
    }
    if (d == nullptr) {
        qCDebug(lcSlskTransfer).noquote() << "file connection from" << link.username << "with an unknown token";
        dropSocket(socket);
        return;
    }
    // The resume offset: what we already have.
    const quint64 offset = static_cast<quint64>(std::max<qint64>(0, d->device->size()));
    if (d->info.size > 0 && offset > d->info.size) {
        finishDownload(d->info.id, DownloadState::Failed, TransferError::LocalError,
                       u"the partial file is larger than the remote file"_s);
        dropSocket(socket);
        return;
    }
    const TransferId id = d->info.id;
    link.kind = FileLink::Kind::Download;
    link.id = id;
    link.deadline = QDeadlineTimer::Forever; // the download's own deadline applies from here
    d->socket = socket;
    d->info.startOffset = offset;
    d->info.received = 0;
    socket->write(file::encodeOffset(offset));
    if (offset >= d->info.size) {
        // Nothing to receive: already complete (or an empty file).
        finishDownload(id, DownloadState::Completed, TransferError::None, {});
        return;
    }
    d->deadline.setRemainingTime(timeouts.stall);
    setDownloadState(*d, DownloadState::Connecting);
    // Bytes that came with the token. Looked up again: a slot may have cancelled the download (and dropped `link`).
    receive(socket, id);
}

void TransferManagerPrivate::receive(QTcpSocket *socket, TransferId id)
{
    const auto it = downloads.find(id);
    const auto link = links.find(socket);
    if (it == downloads.end() || link == links.end()) {
        return;
    }
    Download &d = it->second;
    QByteArray data = std::exchange(link->second.buffer, {});
    data.append(socket->readAll());
    const quint64 remaining = d.info.size - d.info.startOffset - d.info.received;
    if (std::cmp_greater(data.size(), remaining)) {
        data.truncate(static_cast<qsizetype>(remaining)); // anything past the end is ignored
    }
    if (data.isEmpty()) {
        return;
    }
    const qint64 written = d.device->write(data);
    if (written != data.size()) {
        finishDownload(id, DownloadState::Failed, TransferError::LocalError,
                       u"couldn't write the download: %1"_s.arg(d.device->errorString()));
        return;
    }
    d.info.received += static_cast<quint64>(written);
    d.deadline.setRemainingTime(timeouts.stall);
    if (d.info.startOffset + d.info.received >= d.info.size) {
        // We have every byte: the downloader closes the connection (protocol §7).
        finishDownload(id, DownloadState::Completed, TransferError::None, {});
        return;
    }
    markProgress(d);
    if (d.info.state == DownloadState::Connecting) {
        setDownloadState(d, DownloadState::Transferring); // the first bytes
    }
}

// --- uploads ---------------------------------------------------------------------------------------------------------

void TransferManagerPrivate::setUploadState(Upload &u, UploadState state)
{
    u.info.state = state;
    notifyUpload(u.info);
}

void TransferManagerPrivate::finishUpload(TransferId id, UploadState state, TransferError error, const QString &detail,
                                          bool tellDownloader)
{
    const auto it = uploads.find(id);
    if (it == uploads.end()) {
        return;
    }
    Upload u = std::move(it->second);
    uploads.erase(it);
    if (u.socket != nullptr) {
        dropSocket(u.socket);
    }
    if (u.fileRequest != 0 && u.socket == nullptr) {
        peers.cancelFileConnection(u.fileRequest);
    }
    u.info.state = state;
    u.info.error = error;
    if (!detail.isEmpty()) {
        u.info.detail = detail;
    }
    if (tellDownloader) {
        // The downloader re-queues on UploadFailed (or ignores it if it has the file).
        peers.sendPeerMessage(u.info.request.username, frame(peer::UploadFailed{u.info.request.path}),
                              ConnectPurpose::Transfer);
    }
    if (state == UploadState::Completed && u.info.sent > 0) {
        const qint64 ms = std::max<qint64>(1, u.clock.isValid() ? u.clock.elapsed() : 1);
        u.info.averageSpeed = static_cast<quint32>(
            std::min<quint64>((u.info.sent * 1000) / static_cast<quint64>(ms), std::numeric_limits<quint32>::max()));
        // Honest reporting: the server folds it into our average speed (SHARE-3, protocol §7).
        server.sendUploadSpeed(u.info.averageSpeed);
        emitProgress(u);
    }
    qCInfo(lcSlskTransfer).noquote() << "upload" << uploadStateName(state) << ":" << u.info.request.path.display()
                                     << "to" << u.info.request.username << transferErrorName(error) << detail;
    notifyUpload(u.info);
}

void TransferManagerPrivate::onTransferResponse(const QString &username, const peer::TransferResponse &response)
{
    for (auto &[id, u] : uploads) {
        if (u.token != response.token || u.info.request.username != username) {
            continue;
        }
        if (u.info.state != UploadState::Requested) {
            return; // a duplicate answer
        }
        if (!response.allowed) {
            u.info.rejection = classifyRejection(response.reason);
            if (u.info.rejection->reason == RejectionReason::Complete) {
                finishUpload(id, UploadState::Completed, TransferError::None, u"the downloader already has the file"_s,
                             false);
            } else {
                finishUpload(id, UploadState::Declined, TransferError::None, response.reason, false);
            }
            return;
        }
        // The start deadline keeps running: 45 s from TransferRequest to the first byte (Nicotine+).
        u.allowed = true;
        u.fileRequest = peers.openFileConnection(username); // reports on a later turn, never inside the call
        setUploadState(u, UploadState::Connecting);         // last: a slot may cancel the upload
        return;
    }
    qCDebug(lcSlskTransfer).noquote() << "ignored a TransferResponse with an unknown token from" << username;
}

void TransferManagerPrivate::uploadHandshake(QTcpSocket *socket, FileLink &link)
{
    const auto it = uploads.find(link.id);
    if (it == uploads.end()) {
        dropSocket(socket);
        return;
    }
    Upload &u = it->second;
    if (link.buffer.size() < file::kOffsetBytes) {
        return;
    }
    const quint64 offset = file::decodeOffset(link.buffer.first(file::kOffsetBytes)).value_or(0);
    link.buffer.remove(0, file::kOffsetBytes);
    u.offsetReceived = true;
    // An offset past the end (or Soulseek NS's -1 beyond 2 GiB) can't be served.
    if (offset > u.info.request.size) {
        finishUpload(u.info.id, UploadState::Failed, TransferError::ProtocolError, u"resume offset past the end"_s,
                     true);
        return;
    }
    if (!u.source->seek(static_cast<qint64>(offset))) {
        finishUpload(u.info.id, UploadState::Failed, TransferError::LocalError, u"couldn't seek the file"_s, true);
        return;
    }
    const TransferId id = u.info.id;
    u.info.startOffset = offset;
    u.info.sent = 0;
    u.clock.start();
    // Nothing to send when the downloader has it all: it closes.
    u.allQueued = offset == u.info.request.size;
    u.deadline.setRemainingTime(u.allQueued ? timeouts.uploadLinger : timeouts.stall);
    setUploadState(u, UploadState::Transferring);
    pump(id); // looks the upload up again: a slot may have cancelled it
}

void TransferManagerPrivate::pump(TransferId id)
{
    const auto it = uploads.find(id);
    if (it == uploads.end()) {
        return;
    }
    Upload &u = it->second;
    if (u.socket == nullptr || u.info.state != UploadState::Transferring || u.allQueued) {
        return;
    }
    const quint64 total = u.info.request.size - u.info.startOffset;
    const quint64 before = u.info.sent;
    while (u.info.sent < total && u.socket->bytesToWrite() < kWriteWatermark) {
        qint64 want = std::min<qint64>(kChunk, static_cast<qint64>(total - u.info.sent));
        if (speedLimit > 0) {
            want = std::min(want, budget);
            if (want <= 0) {
                break; // the next tick refills the budget
            }
        }
        const QByteArray chunk = u.source->read(want);
        if (chunk.isEmpty()) {
            finishUpload(id, UploadState::Failed, TransferError::LocalError,
                         u"couldn't read the file: %1"_s.arg(u.source->errorString()), true);
            return;
        }
        if (speedLimit > 0) {
            budget -= chunk.size();
        }
        u.socket->write(chunk);
        u.info.sent += static_cast<quint64>(chunk.size());
    }
    if (u.info.sent >= total) {
        u.allQueued = true;
        u.deadline.setRemainingTime(timeouts.uploadLinger);
    }
    if (u.info.sent != before) {
        markProgress(u);
    }
}

void TransferManagerPrivate::pumpAll()
{
    // Round-robin under a speed limit: each upload gets a turn at the shared budget.
    QList<TransferId> ids;
    for (const auto &[id, u] : uploads) {
        ids.append(id);
    }
    for (const TransferId id : std::as_const(ids)) {
        pump(id);
    }
}

// --- F sockets -------------------------------------------------------------------------------------------------------

bool TransferManagerPrivate::expectsFileConnection(const QString &username) const
{
    const bool download = std::ranges::any_of(downloads, [&](const auto &entry) {
        return entry.second.info.request.username == username && entry.second.info.state == DownloadState::Requested;
    });
    const bool upload = std::ranges::any_of(uploads, [&](const auto &entry) {
        return entry.second.info.request.username == username && entry.second.info.state == UploadState::Connecting &&
               entry.second.socket == nullptr;
    });
    return download || upload;
}

void TransferManagerPrivate::onFileConnection(quint64 requestId, const QString &username, QTcpSocket *socket,
                                              const QByteArray &pending)
{
    socket->setParent(q); // ours now
    if (requestId != 0) {
        // The connection we opened for an upload.
        const auto found = std::ranges::find_if(uploads, [&](const auto &entry) {
            return entry.second.fileRequest == requestId && entry.second.socket == nullptr;
        });
        if (found != uploads.end()) {
            adopt(socket, FileLink{FileLink::Kind::Upload, username, found->first, pending, {}});
            return;
        }
        socket->abort();
        socket->deleteLater();
        return;
    }
    // The peer opened it, or we dialled back. A download from that user waiting for its file connection gets it
    // (the token says which); otherwise an upload to them that still waits (Soulseek.NET downloaders open the
    // connection themselves when ours is slow to arrive). With both waiting, the download gets it: a downloader that
    // waits for our token then times out, and our own connection serves the upload.
    const bool downloadWaiting = std::ranges::any_of(downloads, [&](const auto &entry) {
        return entry.second.info.request.username == username && entry.second.info.state == DownloadState::Requested;
    });
    if (downloadWaiting) {
        adopt(socket, FileLink{FileLink::Kind::Unassigned, username, 0, pending, {}});
        return;
    }
    const auto waiting = std::ranges::find_if(uploads, [&](const auto &entry) {
        const Upload &u = entry.second;
        return u.info.request.username == username && u.info.state == UploadState::Connecting && u.socket == nullptr;
    });
    if (waiting != uploads.end()) {
        if (const quint64 request = std::exchange(waiting->second.fileRequest, 0); request != 0) {
            peers.cancelFileConnection(request); // ours isn't needed any more
        }
        adopt(socket, FileLink{FileLink::Kind::Upload, username, waiting->first, pending, {}});
        return;
    }
    qCDebug(lcSlskTransfer).noquote() << "unexpected file connection from" << username;
    socket->abort();
    socket->deleteLater();
}

void TransferManagerPrivate::onFileConnectionFailed(quint64 requestId, PeerError error)
{
    for (auto &[id, u] : uploads) {
        if (u.fileRequest == requestId) {
            u.fileRequest = 0;
            u.info.peerError = error;
            finishUpload(id, UploadState::Failed, TransferError::ConnectionFailed,
                         QString::fromLatin1(peerErrorName(error)), true);
            return;
        }
    }
}

void TransferManagerPrivate::adopt(QTcpSocket *socket, FileLink link)
{
    const FileLink::Kind kind = link.kind;
    const TransferId id = link.id;
    if (kind == FileLink::Kind::Upload && !uploads.contains(id)) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    if (kind == FileLink::Kind::Unassigned) {
        link.deadline.setRemainingTime(timeouts.handshake);
    }
    links.emplace(socket, std::move(link));
    QObject::connect(socket, &QTcpSocket::readyRead, q, [this, socket] { onSocketData(socket); });
    QObject::connect(socket, &QTcpSocket::disconnected, q, [this, socket] { onSocketClosed(socket); });
    QObject::connect(socket, &QTcpSocket::errorOccurred, q, [this, socket] { onSocketClosed(socket); });
    QObject::connect(socket, &QTcpSocket::bytesWritten, q, [this, socket] {
        const auto it = links.find(socket);
        if (it == links.end() || it->second.kind != FileLink::Kind::Upload) {
            return;
        }
        if (const auto u = uploads.find(it->second.id); u != uploads.end()) {
            u->second.deadline.setRemainingTime(u->second.allQueued ? timeouts.uploadLinger : timeouts.stall);
            pump(it->second.id);
        }
    });
    if (kind == FileLink::Kind::Upload) {
        Upload &u = uploads.at(id); // still on the start deadline
        u.socket = socket;
        u.fileRequest = 0;
        socket->write(file::encodeTransferInit(u.token)); // the uploader always sends the token (protocol §7)
    }
    // Bytes that came with the hand-off, or arrived since.
    onSocketData(socket);
}

void TransferManagerPrivate::onSocketData(QTcpSocket *socket)
{
    const auto it = links.find(socket);
    if (it == links.end()) {
        return;
    }
    FileLink &link = it->second;
    switch (link.kind) {
    case FileLink::Kind::Unassigned:
        link.buffer.append(socket->readAll());
        downloadHandshake(socket, link);
        return;
    case FileLink::Kind::Download:
        receive(socket, link.id);
        return;
    case FileLink::Kind::Upload: {
        const auto u = uploads.find(link.id);
        if (u == uploads.end()) {
            return;
        }
        if (!u->second.offsetReceived) {
            link.buffer.append(socket->readAll());
            uploadHandshake(socket, link);
        } else {
            socket->readAll(); // the downloader sends nothing else; drained so it can't pile up
        }
        return;
    }
    }
}

void TransferManagerPrivate::onSocketClosed(QTcpSocket *socket)
{
    const auto it = links.find(socket);
    if (it == links.end()) {
        return;
    }
    const FileLink link = it->second;
    switch (link.kind) {
    case FileLink::Kind::Unassigned:
        dropSocket(socket);
        return;
    case FileLink::Kind::Download: {
        const auto d = downloads.find(link.id);
        if (d != downloads.end()) {
            receive(socket, link.id); // whatever arrived with the close
        }
        if (downloads.contains(link.id)) {
            finishDownload(link.id, DownloadState::Failed, TransferError::ConnectionLost,
                           u"the uploader closed the connection early"_s);
        }
        return;
    }
    case FileLink::Kind::Upload: {
        const auto u = uploads.find(link.id);
        if (u == uploads.end()) {
            dropSocket(socket);
            return;
        }
        // The downloader closes once it has everything; closing earlier means the transfer broke.
        const bool complete = u->second.allQueued && socket->bytesToWrite() == 0;
        if (complete) {
            finishUpload(link.id, UploadState::Completed, TransferError::None, {}, false);
        } else {
            finishUpload(link.id, UploadState::Failed, TransferError::ConnectionLost,
                         u"the downloader closed the connection early"_s, true);
        }
        return;
    }
    }
}

void TransferManagerPrivate::dropSocket(QTcpSocket *socket, bool flush)
{
    links.erase(socket);
    socket->disconnect(q);
    if (flush) {
        // What we wrote still goes out (a resume offset equal to the size), then the connection closes; a peer that
        // doesn't take it in time is cut off.
        QObject::connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(kFlushTimeout, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        socket->disconnectFromHost();
    } else {
        socket->abort();
        socket->deleteLater();
    }
    for (auto &[id, d] : downloads) {
        if (d.socket == socket) {
            d.socket = nullptr;
        }
    }
    for (auto &[id, u] : uploads) {
        if (u.socket == socket) {
            u.socket = nullptr;
        }
    }
}

void TransferManagerPrivate::onSweep()
{
    // Inbound sockets that never said which download they carry.
    QList<QTcpSocket *> silent;
    for (const auto &[socket, link] : links) {
        if (link.kind == FileLink::Kind::Unassigned && link.deadline.hasExpired()) {
            silent.append(socket);
        }
    }
    for (QTcpSocket *socket : std::as_const(silent)) {
        if (const auto it = links.find(socket); it != links.end()) {
            qCDebug(lcSlskTransfer).noquote() << "file connection from" << it->second.username << "sent no token";
            dropSocket(socket);
        }
    }
    QList<TransferId> expiredDownloads;
    for (const auto &[id, d] : downloads) {
        if (d.info.state != DownloadState::Queued && d.deadline.hasExpired()) {
            expiredDownloads.append(id);
        }
    }
    for (const TransferId id : std::as_const(expiredDownloads)) {
        const auto it = downloads.find(id);
        if (it == downloads.end()) {
            continue;
        }
        finishDownload(id, DownloadState::Failed, TransferError::Timeout, timeoutDetail(it->second.info.state));
    }
    QList<TransferId> expiredUploads;
    for (const auto &[id, u] : uploads) {
        if (u.deadline.hasExpired()) {
            expiredUploads.append(id);
        }
    }
    for (const TransferId id : std::as_const(expiredUploads)) {
        const auto it = uploads.find(id);
        if (it == uploads.end()) {
            continue;
        }
        const Upload &u = it->second;
        if (u.allQueued && u.socket != nullptr && u.socket->bytesToWrite() == 0) {
            // Every byte went out and the downloader didn't close within the linger: done anyway.
            finishUpload(id, UploadState::Completed, TransferError::None, {}, false);
            continue;
        }
        const bool tellDownloader = u.allowed;
        finishUpload(id, UploadState::Failed, TransferError::Timeout, timeoutDetail(u), tellDownloader);
    }
}

} // namespace internal

// --- TransferManager -------------------------------------------------------------------------------------------------

using internal::Download;
using internal::Upload;

TransferManager::TransferManager(PeerNetwork &peers, ServerSession &server, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<internal::TransferManagerPrivate>(this, peers, server))
{
    registerMetaTypes();
    connect(&peers, &PeerNetwork::transferRequestReceived, this,
            [this](const QString &user, const peer::TransferRequest &m) { d->onTransferRequest(user, m); });
    connect(&peers, &PeerNetwork::transferResponseReceived, this,
            [this](const QString &user, const peer::TransferResponse &m) { d->onTransferResponse(user, m); });
    connect(&peers, &PeerNetwork::queueUploadReceived, this, [this](const QString &user, const peer::QueueUpload &m) {
        d->notify([this, user, path = m.path] { Q_EMIT uploadRequested(user, path, false); });
    });
    connect(&peers, &PeerNetwork::placeInQueueRequestReceived, this,
            [this](const QString &user, const peer::PlaceInQueueRequest &m) {
                d->notify([this, user, path = m.path] { Q_EMIT placeInQueueRequested(user, path); });
            });
    connect(&peers, &PeerNetwork::placeInQueueResponseReceived, this,
            [this](const QString &user, const peer::PlaceInQueueResponse &m) { d->onPlaceInQueue(user, m); });
    connect(&peers, &PeerNetwork::uploadFailedReceived, this,
            [this](const QString &user, const peer::UploadFailed &m) { d->onUploadFailed(user, m); });
    connect(&peers, &PeerNetwork::uploadDeniedReceived, this,
            [this](const QString &user, const peer::UploadDenied &m) { d->onUploadDenied(user, m); });
    connect(&peers, &PeerNetwork::fileConnectionReady, this,
            [this](quint64 request, const QString &user, QTcpSocket *socket, const QByteArray &pending) {
                d->onFileConnection(request, user, socket, pending);
            });
    connect(&peers, &PeerNetwork::fileConnectionFailed, this,
            [this](quint64 request, const QString &, PeerError error) { d->onFileConnectionFailed(request, error); });
    peers.setFileConnectionFilter([guard = QPointer<TransferManager>(this)](const QString &user) {
        return guard && guard->d->expectsFileConnection(user);
    });
    connect(d->sweep, &QTimer::timeout, this, [this] { d->onSweep(); });
    connect(d->progressTimer, &QTimer::timeout, this, [this] { d->flushProgress(); });
    connect(d->budgetTimer, &QTimer::timeout, this, [this] {
        // Refill the shared upload budget; at most two ticks' worth can accumulate.
        const qint64 perTick = std::max<qint64>(1, d->speedLimit * internal::kBudgetTick.count() / 1000);
        d->budget = std::min(d->budget + perTick, 2 * perTick);
        d->pumpAll();
    });
    d->sweep->start(250ms);
}

TransferManager::~TransferManager()
{
    // No signals from here on. Partial downloads are closed (and so flushed) through the storage; upload devices are
    // core/'s to close.
    d->peers.setFileConnectionFilter({});
    for (auto &[socket, link] : d->links) {
        socket->disconnect(this);
        socket->abort();
    }
    if (d->storage != nullptr) {
        for (const auto &[id, download] : d->downloads) {
            if (download.device != nullptr) {
                d->storage->closeDownload(download.info, download.device);
            }
        }
    }
}

void TransferManager::setTimeouts(const Timeouts &timeouts)
{
    d->timeouts = timeouts;
    d->progressTimer->setInterval(timeouts.progress);
    const auto shortest =
        std::min({timeouts.requested, timeouts.handshake, timeouts.stall, timeouts.uploadStart, timeouts.uploadLinger});
    d->sweep->start(std::clamp<std::chrono::milliseconds>(shortest / 4, 10ms, 250ms));
}

void TransferManager::setDownloadStorage(DownloadStorage *storage)
{
    d->storage = storage;
}

void TransferManager::setUploadSpeedLimit(qint64 bytesPerSecond)
{
    d->speedLimit = std::max<qint64>(0, bytesPerSecond);
    if (d->speedLimit > 0) {
        d->budget = 0;
        d->budgetTimer->start(internal::kBudgetTick);
    } else {
        d->budgetTimer->stop();
        d->pumpAll();
    }
}

TransferId TransferManager::download(const DownloadRequest &request, governor::Permit queueDownload)
{
    if (request.username.isEmpty() || request.path.isEmpty() || !queueDownload.isValid() ||
        queueDownload.operation() != governor::Operation::QueueDownload ||
        !queueDownload.consume(governor::Operation::QueueDownload)) {
        return 0;
    }
    if (const Download *existing = d->findDownload(request.username, request.path)) {
        return existing->info.id;
    }
    const TransferId id = d->nextId++;
    Download download;
    download.info.id = id;
    download.info.request = request;
    download.info.size = request.expectedSize;
    download.info.state = DownloadState::Queued;
    const DownloadInfo queued = download.info;
    d->downloads.emplace(id, std::move(download));
    // A failure to deliver QueueUpload is reported on a later turn, never inside this call.
    const QPointer<TransferManager> guard(this);
    d->peers.sendPeerMessage(request.username, frame(peer::QueueUpload{request.path}), ConnectPurpose::Request,
                             [guard, id](PeerError error) {
                                 if (!guard || error == PeerError::None) {
                                     return;
                                 }
                                 const auto it = guard->d->downloads.find(id);
                                 if (it == guard->d->downloads.end() ||
                                     it->second.info.state != DownloadState::Queued) {
                                     return;
                                 }
                                 it->second.info.peerError = error;
                                 guard->d->finishDownload(id, DownloadState::Failed, TransferError::ConnectionFailed,
                                                          QString::fromLatin1(peerErrorName(error)));
                             });
    d->notifyDownload(queued); // last: a slot may cancel it
    return id;
}

bool TransferManager::requestPlaceInQueue(TransferId id, governor::Permit placeInQueue)
{
    const auto it = d->downloads.find(id);
    if (it == d->downloads.end() || it->second.info.state != DownloadState::Queued || !placeInQueue.isValid() ||
        placeInQueue.operation() != governor::Operation::PlaceInQueue ||
        !placeInQueue.consume(governor::Operation::PlaceInQueue)) {
        return false;
    }
    d->peers.sendPeerMessage(it->second.info.request.username,
                             frame(peer::PlaceInQueueRequest{it->second.info.request.path}), ConnectPurpose::Request);
    return true;
}

void TransferManager::cancelDownload(TransferId id)
{
    // Marked cancelled before its connection closes, so the uploader's UploadFailed echo finds nothing to fail; a
    // later TransferRequest for it is answered "Cancelled".
    d->finishDownload(id, DownloadState::Cancelled, TransferError::None, {});
}

std::optional<DownloadInfo> TransferManager::downloadInfo(TransferId id) const
{
    const auto it = d->downloads.find(id);
    return it == d->downloads.end() ? std::nullopt : std::optional<DownloadInfo>(it->second.info);
}

QList<DownloadInfo> TransferManager::downloads() const
{
    QList<DownloadInfo> list;
    for (const auto &[id, download] : d->downloads) {
        list.append(download.info);
    }
    return list;
}

TransferId TransferManager::startUpload(const UploadRequest &request, QIODevice *source)
{
    if (request.username.isEmpty() || request.path.isEmpty() || source == nullptr || !source->isReadable() ||
        source->isSequential()) {
        return 0;
    }
    const TransferId id = d->nextId++;
    Upload upload;
    upload.info.id = id;
    upload.info.request = request;
    upload.info.state = UploadState::Requested;
    upload.source = source;
    upload.token = d->transferTokens.next([this](quint32 token) {
        return std::ranges::any_of(d->uploads, [token](const auto &entry) { return entry.second.token == token; });
    });
    upload.deadline.setRemainingTime(d->timeouts.uploadStart);
    const quint32 token = upload.token;
    const UploadInfo requested = upload.info;
    d->uploads.emplace(id, std::move(upload));
    const QPointer<TransferManager> guard(this);
    d->peers.sendPeerMessage(request.username,
                             frame(peer::TransferRequest{TransferDirection::Upload, token, request.path, request.size}),
                             ConnectPurpose::Transfer, [guard, id](PeerError error) {
                                 if (!guard || error == PeerError::None) {
                                     return;
                                 }
                                 const auto it = guard->d->uploads.find(id);
                                 if (it == guard->d->uploads.end()) {
                                     return;
                                 }
                                 it->second.info.peerError = error;
                                 guard->d->finishUpload(id, UploadState::Failed, TransferError::ConnectionFailed,
                                                        QString::fromLatin1(peerErrorName(error)), false);
                             });
    d->notifyUpload(requested); // last: a slot may cancel it
    return id;
}

void TransferManager::cancelUpload(TransferId id, const QString &denyReason)
{
    const auto it = d->uploads.find(id);
    if (it == d->uploads.end()) {
        return;
    }
    const UploadRequest request = it->second.info.request;
    const bool allowed = it->second.allowed;
    d->finishUpload(id, UploadState::Cancelled, TransferError::None, denyReason, allowed && denyReason.isEmpty());
    if (!denyReason.isEmpty()) {
        denyUpload(request.username, request.path, denyReason);
    }
}

std::optional<UploadInfo> TransferManager::uploadInfo(TransferId id) const
{
    const auto it = d->uploads.find(id);
    return it == d->uploads.end() ? std::nullopt : std::optional<UploadInfo>(it->second.info);
}

QList<UploadInfo> TransferManager::uploads() const
{
    QList<UploadInfo> list;
    for (const auto &[id, upload] : d->uploads) {
        list.append(upload.info);
    }
    return list;
}

void TransferManager::denyUpload(const QString &username, const RemotePath &path, const QString &reason)
{
    d->peers.sendPeerMessage(username, frame(peer::UploadDenied{path, reason}), ConnectPurpose::Transfer);
}

void TransferManager::sendPlaceInQueue(const QString &username, const RemotePath &path, quint32 place)
{
    d->peers.sendPeerMessage(username, frame(peer::PlaceInQueueResponse{path, place}), ConnectPurpose::Transfer);
}

} // namespace selecta::slsk
