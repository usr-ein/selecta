// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "core/Downloads.h"
#include "slsk/TransferManager.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QFile;
class QTimer;

namespace selecta::governor {
class Governor;
}
namespace selecta::storage {
class Database;
}

namespace selecta::core {

class ActivityLog;
class NetworkService;
struct NetworkStatus;

// A file to download, as Get tracks (or a manual download) asks for it.
struct DownloadSpec
{
    QString peer;
    QByteArray remotePath;
    qint64 size = 0;
    std::optional<FileChoice> file;
    std::optional<meta::Release> release;
    std::optional<qint64> getLine;
    std::optional<qint64> album; // part of an album download: it arrives with the album's other files
};

// A preview (PRD PREV-1, PREV-6, PREV-7): a file downloaded like any other, but into the preview cache, readable while
// it arrives. Previews aren't saved; a complete one is kept for the session, so getting the same file reuses it.
struct Preview
{
    enum class State { Queued, Transferring, Complete, Failed, Cancelled };

    qint64 id = 0;
    QString peer;
    QByteArray remotePath;
    qint64 size = 0;
    std::optional<qint64> getLine;
    State state = State::Queued;
    QString message; // what is happening or what went wrong, in words
    qint64 received = 0;

    bool isFinished() const { return state != State::Queued && state != State::Transferring; }
};

// Downloads (PRD DL-1, DL-2) on the engine thread: a queue kept in the database, so it survives a restart, driven
// through slsk's TransferManager within the governor's limits (ADR-006). Every download asks the governor for a
// QueueDownload permit before it is queued at the peer, and the governor hears about every start, end and denial.
//
// A download waits while Selecta is offline or paced; transient failures (unreachable peer, lost connection, stalls,
// a peer's full queue) wait and try again after 3, 10 and 30 minutes, resuming the partial file; anything else fails
// for good and says why. A complete file is announced with completed(): the arrival pipeline converts and publishes
// it, then reports back with arrived() or arrivalFailed().
//
// Files: partial and complete originals live in Options::incomingDir, one folder per download, until the arrival
// pipeline is done with them.
//
// Threading: engine thread. Ownership: the Governor, the activity log and the database outlive it. The NetworkService
// (and its TransferManager, which writes into this service's files) must be destroyed after shutdown() and before this
// service; Engine::stop() does it in that order.
class TransferService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;

    // The preview cache's bound (PREV-8, CACHE-1): peer previews and YouTube audio together.
    static constexpr qint64 kPreviewCacheBytes = qint64{2} * 1000 * 1000 * 1000;

    struct Options
    {
        QString incomingDir;
        // Waits before each retry of a transient failure; the number of entries is the number of retries. Default:
        // 3, 10 and 30 minutes (Limits.h kDownloadRetries). Tests shorten them.
        QList<qint64> retryDelaysMs;
        qint64 pacedRetryMs = 60'000; // a download the governor turned away without saying when asks again after this
        QString previewDir;           // previews; emptied at start (previews don't outlive a session)
    };

    // A preview that started: its id and what a player reads while the bytes arrive.
    struct PreviewStart
    {
        qint64 id = 0;
        std::shared_ptr<audio::ByteSource> source;
    };

    TransferService(storage::Database &db, governor::Governor &governor, NetworkService &network, ActivityLog &activity,
                    Options options, WallClock now = {}, QObject *parent = nullptr);
    ~TransferService() override;
    TransferService(const TransferService &) = delete;
    TransferService &operator=(const TransferService &) = delete;
    TransferService(TransferService &&) = delete;
    TransferService &operator=(TransferService &&) = delete;

    // Restores the downloads that hadn't finished at the last quit, and starts what can start. Complete files that
    // were being converted are announced again with completed().
    void load();

    // Wants a file. Returns its download id; a file already being downloaded returns that download. nullopt only when
    // the database refuses (logged).
    std::optional<qint64> enqueue(const DownloadSpec &spec);
    // Stops a download at any stage before it has arrived. Its partial file is deleted.
    void cancel(qint64 id);
    // A failed download tries again, with its retries renewed.
    void retry(qint64 id);

    // The arrival pipeline's answers for a completed download.
    void arrived(qint64 id, qint64 inboxItem);
    void arrivalFailed(qint64 id, const QString &message);

    std::optional<Download> download(qint64 id);
    QList<Download> unfinished() const;

    // --- previews (one at a time) ---
    // Starts a preview of a file, replacing the one playing, and returns the source to play while it arrives. A file
    // already complete in the preview cache plays from there at once. nullopt, with `why`, when it can't start
    // (offline, the governor's preview pacing or limits, a file without a known size, or a file already downloading).
    std::optional<PreviewStart> startPreview(const DownloadSpec &spec, QString *why = nullptr);
    // Stops a preview. One that has nearly arrived (Limits.h kPreviewFinishPercent) completes instead, for a Get to
    // reuse.
    void stopPreview(qint64 id);
    std::optional<Preview> preview(qint64 id) const;

    // Stops listening to the transfer manager before the network goes; unfinished downloads stay saved for the next
    // launch, and open files are closed by the manager as it is destroyed. Nothing else may be called afterwards.
    void shutdown();

Q_SIGNALS:
    void changed(const selecta::core::Download &download);
    // Bytes on disk (a resumed part included) and the file's size; about four times a second while transferring.
    void progress(qint64 id, qint64 bytesDone, qint64 bytesTotal);
    // The file is complete on disk (download.localPath): the arrival pipeline takes it from here.
    void completed(const selecta::core::Download &download);
    void previewChanged(const selecta::core::Preview &preview);

private:
    class Storage;
    friend class Storage;

    void startWaiting();
    void tryStart(Download &download);
    void transferChanged(const slsk::DownloadInfo &info);
    void transferProgress(slsk::TransferId id, quint64 done, quint64 total);
    void networkChanged(const NetworkStatus &status);
    void wait(Download &download, qint64 delayMs, const QString &message);
    void failed(Download &download, const slsk::DownloadInfo &info);
    void finish(Download &download, Download::State state, const QString &message);
    // The transfer behind a download ended or is being dropped: tells the governor, then forgets the mapping.
    void release(Download &download, qint64 bytes);
    void forget(qint64 id);
    void save(Download &download);
    bool isDue(qint64 id) const;
    void scheduleRetries();
    void deleteFiles(const Download &download);
    struct PreviewSlot;
    void previewTransferChanged(qint64 id, const slsk::DownloadInfo &info);
    void releasePreview(PreviewSlot &slot, qint64 bytes);
    void finishPreview(PreviewSlot &slot, Preview::State state, const QString &message);
    std::optional<Download> reusePreview(const DownloadSpec &spec);
    // A preview still arriving becomes the download when it completes, instead of a second transfer (PREV-5).
    std::optional<qint64> keepPreview(const DownloadSpec &spec);
    // Hands a complete preview file to the arrival pipeline as `download`'s file; false when it can't be copied.
    bool deliverFromPreview(Download &download, const QString &previewPath);
    // A kept preview ended: its download takes the file, or asks the peer itself when the preview failed.
    void keptPreviewEnded(qint64 downloadId, const QString &completePath);
    // Deletes the oldest preview files once the cache is over its size (PREV-8); files in use stay.
    void trimPreviewCache();
    static QString previewKey(const QString &peer, const QByteArray &remotePath);
    QString folderOf(qint64 id) const;
    QString partialPathOf(const Download &download) const;
    QIODevice *openFile(const slsk::DownloadInfo &info);
    void closeFile(const slsk::DownloadInfo &info, QIODevice *device);

    storage::Database &m_db;
    governor::Governor &m_governor;
    // cppcheck-suppress uninitMemberVarNoCtor ; bound in the constructor's initialiser list
    NetworkService &m_network;
    ActivityLog &m_activity;
    Options m_options;
    WallClock m_now;
    DownloadStore m_store;
    std::unique_ptr<Storage> m_storage;
    QHash<qint64, Download> m_live;                        // unfinished downloads, by id
    QHash<qint64, slsk::TransferId> m_transferOf;          // download → its transfer, while queued or transferring
    QHash<slsk::TransferId, qint64> m_downloadOf;          // and back
    QSet<qint64> m_active;                                 // downloads the governor counts as started
    QHash<slsk::TransferId, QFile *> m_files;              // open partial files, owned
    QHash<slsk::TransferId, QIODevice *> m_previewWriters; // open preview files, owned
    QHash<qint64, QDeadlineTimer> m_retryAt;               // when a waiting download may ask again (monotonic)
    QTimer *m_retryTimer = nullptr;
    QHash<qint64, std::shared_ptr<PreviewSlot>> m_previews; // by preview id, until finished and replaced
    QHash<slsk::TransferId, qint64> m_previewOf;            // a preview's transfer → its id
    QHash<QString, QString> m_completedPreviews;            // previewKey → the complete file in the preview cache
    QSet<qint64> m_awaitingPreview;                         // downloads that a kept preview will hand its file to
    qint64 m_nextPreviewId = 1;
    bool m_shutDown = false;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::Preview)
Q_DECLARE_METATYPE(std::shared_ptr<selecta::audio::ByteSource>)
