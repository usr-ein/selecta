// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "slsk/PeerNetwork.h"
#include "slsk/Protocol.h"
#include "slsk/Types.h"

#include <QObject>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>

class QIODevice;

namespace selecta::slsk {

class ServerSession;

namespace internal {
class TransferManagerPrivate;
} // namespace internal

using TransferId = quint64; // never 0

enum class DownloadState {
    Queued,       // QueueUpload sent; we wait in the peer's queue (possibly for hours)
    Requested,    // the peer sent TransferRequest and we allowed it; waiting for its `F` connection and token
    Connecting,   // the `F` connection is up and has our resume offset; waiting for the first bytes
    Transferring, // file bytes are flowing
    Completed,    // terminal: every byte arrived
    Failed,       // terminal: see TransferError
    Cancelled,    // terminal: cancelled locally
};

enum class UploadState {
    Requested,    // TransferRequest sent; waiting for the downloader's answer
    Connecting,   // the downloader allowed it: opening the `F` connection, exchanging token and offset
    Transferring, // file bytes are flowing
    Completed,    // terminal: every byte was sent (or the downloader already had the file)
    Failed,       // terminal: see TransferError
    Cancelled,    // terminal: cancelled locally
    Declined,     // terminal: the downloader answered no (`rejection` holds its reason)
};

enum class TransferError {
    None,
    ConnectionFailed, // the peer couldn't be reached (peerError says why)
    Denied,           // the uploader refused the file (rejection holds the exact text and its class)
    RemoteFailed,     // the uploader reported UploadFailed
    ConnectionLost,   // the `F` connection closed before the last byte
    Timeout,          // the `F` connection, the handshake or the data stalled past its timeout
    LocalError,       // our device couldn't be opened, read or written; or the partial file is larger than the remote
    ProtocolError,    // the peer broke the protocol (an offset past the end, a wrong token)
};

const char *downloadStateName(DownloadState state);
const char *uploadStateName(UploadState state);
const char *transferErrorName(TransferError error);

struct DownloadRequest
{
    QString username;
    RemotePath path;          // echoed back exactly as the peer sent it
    quint64 expectedSize = 0; // from the search result; used when the uploader reports 0 (SoulseekQt, > 2 GiB)
    bool preview = false;     // an ordinary download, flagged for per-peer statistics (PREV-7)

    friend bool operator==(const DownloadRequest &, const DownloadRequest &) = default;
};

struct DownloadInfo
{
    TransferId id = 0;
    DownloadRequest request;
    DownloadState state = DownloadState::Queued;
    quint64 size = 0;        // the file's size, once known
    quint64 startOffset = 0; // bytes we already had when the transfer started (resume)
    quint64 received = 0;    // bytes received in this transfer
    std::optional<quint32> placeInQueue;
    TransferError error = TransferError::None;
    PeerError peerError = PeerError::None; // with ConnectionFailed
    std::optional<Rejection> rejection;    // with Denied
    QString detail;                        // human-readable, for the log

    bool isTerminal() const;
};

struct UploadRequest
{
    QString username;
    RemotePath path; // exactly as the downloader asked for it
    quint64 size = 0;

    friend bool operator==(const UploadRequest &, const UploadRequest &) = default;
};

struct UploadInfo
{
    TransferId id = 0;
    UploadRequest request;
    UploadState state = UploadState::Requested;
    quint64 startOffset = 0;  // the downloader's resume offset
    quint64 sent = 0;         // bytes sent in this transfer
    quint32 averageSpeed = 0; // bytes per second over the transfer, once completed (reported to the server)
    TransferError error = TransferError::None;
    PeerError peerError = PeerError::None;
    std::optional<Rejection> rejection; // with Declined
    QString detail;

    bool isTerminal() const;
};

// Where downloads are written. Implemented by core/, which owns the files (slsk never touches files directly).
//
// Threading: called synchronously on the engine thread; it must not call back into the TransferManager. Ownership:
// core/ owns the devices; the storage outlives the TransferManager (or is unset first).
class DownloadStorage
{
public:
    DownloadStorage() = default;
    DownloadStorage(const DownloadStorage &) = delete;
    DownloadStorage &operator=(const DownloadStorage &) = delete;
    virtual ~DownloadStorage();

    // Called when the uploader is ready to send: the partial file of `download`, open for appending. Its size() is the
    // resume offset sent to the uploader. nullptr refuses: the transfer is declined ("Cancelled") and the download
    // fails with LocalError.
    virtual QIODevice *openDownload(const DownloadInfo &download) = 0;
    // The download no longer writes to `device` (it reached a terminal state, or the manager is being destroyed).
    // Flush and close it.
    virtual void closeDownload(const DownloadInfo &download, QIODevice *device) = 0;
};

// Transfers (references/soulseek-protocol.md §6.4, §7), as explicit state machines.
//
// Downloads: download() sends QueueUpload (needs a QueueDownload permit). When the uploader sends TransferRequest, the
// download is allowed if it is still wanted (its device comes from DownloadStorage), and the uploader's `F` connection
// is accepted from either direction: direct, or our dial-back after its ConnectToPeer. We send the resume offset (the
// device's size), write the bytes to the device and close the connection after the last one. A cancelled download is
// marked cancelled before its `F` connection closes; the UploadFailed that the uploader sends afterwards is ignored,
// and a later TransferRequest for a queued download we withdrew is answered "Cancelled". Unsolicited TransferRequests
// are answered "Cancelled" too.
//
// Uploads: queue requests from peers (QueueUpload, and the legacy downloader-sent TransferRequest, which is answered
// "Queued") are signalled to core/, which keeps the upload queue (ADR-012) and calls startUpload() when a slot frees.
// We send TransferRequest, open the `F` connection when allowed, send the token, validate the downloader's offset,
// stream from the device core/ opened, and wait for the downloader to close. A completed upload reports its speed to
// the server (SendUploadSpeed); a broken one tells the downloader (UploadFailed).
//
// Progress is coalesced: at most one progress signal per transfer every protocol::kProgressInterval (~4 a second),
// carrying the latest numbers (a trailing one too, so a stall still shows where it stopped), and a final one when a
// transfer completes; none after a transfer's terminal signal. Every wait has a timeout.
//
// Slots may call back into the manager (cancel, start another transfer), and every receiver still sees the changes in
// the order they happened: a signal caused inside a slot is delivered once the current one has reached every
// receiver. Destroying the manager emits nothing.
//
// Threading: lives on the engine thread and is used only from it. Ownership: a QObject; owns its `F` sockets. Not
// owned: the PeerNetwork and ServerSession (they must outlive it), the DownloadStorage, and upload devices (core/
// closes them after the terminal uploadChanged()).
class TransferManager : public QObject
{
    Q_OBJECT

public:
    struct Timeouts
    {
        std::chrono::milliseconds requested = protocol::kFileRequestedTimeout;
        std::chrono::milliseconds handshake = protocol::kFileHandshakeTimeout;
        std::chrono::milliseconds stall = protocol::kFileStallTimeout;
        std::chrono::milliseconds uploadStart = protocol::kUploadStartTimeout;
        std::chrono::milliseconds uploadLinger = protocol::kUploadLinger;
        std::chrono::milliseconds progress = protocol::kProgressInterval;
    };

    TransferManager(PeerNetwork &peers, ServerSession &server, QObject *parent = nullptr);
    ~TransferManager() override;
    TransferManager(const TransferManager &) = delete;
    TransferManager &operator=(const TransferManager &) = delete;

    void setTimeouts(const Timeouts &timeouts);
    void setDownloadStorage(DownloadStorage *storage);
    // Total upload bandwidth in bytes per second, shared by active uploads; 0 for none.
    void setUploadSpeedLimit(qint64 bytesPerSecond);

    // --- downloads ---------------------------------------------------------------------------------------------------

    // Queues a download at the peer. Returns its id, or 0 (permit dropped) when the permit isn't a QueueDownload one or
    // the request has no username or path. Asking again for a file that is already being downloaded returns the
    // existing id.
    TransferId download(const DownloadRequest &request, governor::Permit queueDownload);
    // Asks the uploader for our place in its queue (PlaceInQueueRequest). False when the download isn't Queued.
    bool requestPlaceInQueue(TransferId id, governor::Permit placeInQueue);
    // Cancels at any time. A transfer in progress stops at once (its `F` connection closes).
    void cancelDownload(TransferId id);
    // nullopt once the download is terminal: the last downloadChanged() carried its final state.
    std::optional<DownloadInfo> downloadInfo(TransferId id) const;
    QList<DownloadInfo> downloads() const; // the ones not yet terminal

    // --- uploads -----------------------------------------------------------------------------------------------------

    // Starts an upload that core/ dequeued. `source` is open for reading and seekable, and its size is `request.size`;
    // core/ keeps ownership and closes it after the terminal uploadChanged(). Returns 0 when the request is invalid.
    TransferId startUpload(const UploadRequest &request, QIODevice *source);
    // Stops an upload. With a reason, the downloader is told with UploadDenied (e.g. "Cancelled", "Banned"), which
    // also tells it not to re-queue; without one, with UploadFailed (it may re-queue).
    void cancelUpload(TransferId id, const QString &denyReason = {});
    // nullopt once the upload is terminal: the last uploadChanged() carried its final state.
    std::optional<UploadInfo> uploadInfo(TransferId id) const;
    QList<UploadInfo> uploads() const; // the ones not yet terminal

    // Answers to queue requests (core/'s upload queue decides).
    void denyUpload(const QString &username, const RemotePath &path, const QString &reason);
    void sendPlaceInQueue(const QString &username, const RemotePath &path, quint32 place);

Q_SIGNALS:
    void downloadChanged(const selecta::slsk::DownloadInfo &info); // every state change and place-in-queue answer
    // Bytes of the file we have (a resumed part included) and its size (0 when unknown). Coalesced, see above.
    void downloadProgress(selecta::slsk::TransferId id, quint64 bytesDone, quint64 bytesTotal);
    void uploadChanged(const selecta::slsk::UploadInfo &info);
    // Bytes of the file sent to the downloader (its resume offset included) and the file's size. Coalesced.
    void uploadProgress(selecta::slsk::TransferId id, quint64 bytesDone, quint64 bytesTotal);

    // A peer wants a file from us: QueueUpload, or (legacy) TransferRequest direction 0, already answered "Queued".
    // core/ checks the share index and its limits, then denyUpload() or, when a slot frees, startUpload().
    void uploadRequested(const QString &username, const selecta::slsk::RemotePath &path, bool legacy);
    void placeInQueueRequested(const QString &username, const selecta::slsk::RemotePath &path);

private:
    friend class internal::TransferManagerPrivate;
    std::unique_ptr<internal::TransferManagerPrivate> d;
};

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::DownloadState)
Q_DECLARE_METATYPE(selecta::slsk::UploadState)
Q_DECLARE_METATYPE(selecta::slsk::TransferError)
Q_DECLARE_METATYPE(selecta::slsk::DownloadInfo)
Q_DECLARE_METATYPE(selecta::slsk::UploadInfo)
