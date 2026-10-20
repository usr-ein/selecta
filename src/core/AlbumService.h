// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/AlbumArrival.h"
#include "core/Albums.h"
#include "core/Downloads.h"
#include "core/InboxService.h"
#include "core/SearchService.h"
#include "library/FileOps.h"
#include "slsk/PeerMessages.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QTimer;

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

class ActivityLog;
class MetaServices;
class NetworkService;
class TransferService;

// What an album download did to the singles it supersedes (DL-7, DL-8), for a notification with Undo.
struct SupersedeReport
{
    qint64 albumId = 0;
    QString album;                  // the album's title
    QStringList trashed;            // Inbox singles moved to the Trash, as "Artist – Title"
    QStringList flagged;            // pairs only flagged: filed singles, uncertain matches
    bool confirmationAsked = false; // the circuit breaker held back the trash: every pair is flagged
};

// Album downloads (PRD DL-1, DL-3, DL-7, DL-8, DL-12) on the engine thread. A remote folder is listed by its peer
// (FolderContents, through the governor), and its audio files and images are downloaded through the transfers, each
// a download of its own. Once every one of them has arrived or failed for good, the album's track list is looked up
// (Spotify, from the release the user picked) and its cover fetched, and a worker makes the files one Inbox album
// (arriveAlbum). Then the album's tracks are checked against the singles: one still in the Inbox goes to the Trash
// (DL-7, with Undo), one filed in the collection is only flagged (DL-8).
//
// Threading: engine thread; album arrivals run on a worker and come back through a mutex-guarded mailbox. Ownership:
// every collaborator outlives it; the destructor cancels arrivals in flight and waits for them.
class AlbumService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;

    struct Options
    {
        QString stagingDir;
        QString encodedBy;
        std::optional<int> session;
        std::chrono::milliseconds listingTimeout{30'000};  // then the files known from the search are used
        std::chrono::milliseconds metadataTimeout{20'000}; // then the album is made with what is known by then
    };

    AlbumService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                 TransferService &transfers, InboxService &inbox, ActivityLog &activity,
                 library::FileOps::TrashFunction trash, Options options, WallClock now = {}, QObject *parent = nullptr);
    ~AlbumService() override;
    AlbumService(const AlbumService &) = delete;
    AlbumService &operator=(const AlbumService &) = delete;
    AlbumService(AlbumService &&) = delete;
    AlbumService &operator=(AlbumService &&) = delete;

    // Where track lists and covers come from. Without it, albums are tagged from their files and the release given.
    void setMeta(MetaServices *meta) { m_meta = meta; }

    // Downloads `folder` of `peer` whole, into an Inbox album. `known`: files of it from a search, used when the peer
    // doesn't list the folder; `release`: a track of the album, to tag from. Returns the album's id.
    std::optional<qint64> download(const QString &peer, const QByteArray &folder, const QList<SearchHit> &known,
                                   const std::optional<meta::Release> &release);
    void cancel(qint64 albumId);
    // Albums that weren't done when Selecta quit go on (their files are the transfers' to resume).
    void restore();
    // Puts back the singles an album's supersede moved to the Trash.
    void undoSupersede(qint64 albumId);

    // --- fed by the transfers and the Inbox ---
    // An album file has arrived whole (TransferService::completed): the album is made once every file is in.
    void fileCompleted(const selecta::core::Download &download);
    // An album file changed state: a failure or a cancellation may complete the album.
    void downloadChanged(const selecta::core::Download &download);
    // The peer answered a folder listing (PeerNetwork::folderContentsReceived).
    void folderContentsReceived(const QString &username, const selecta::slsk::peer::FolderContentsResponse &contents);
    // An Inbox item went to the Trash (InboxService::trashed): kept for Undo when a supersede sent it there.
    void inboxItemTrashed(const selecta::core::TrashedItem &trashed);

    std::optional<AlbumDownload> album(qint64 id);
    QList<AlbumDownload> unfinished();
    // Blocks until the arrivals in flight are done and recorded. For tests.
    void waitForIdle();

Q_SIGNALS:
    void albumChanged(const selecta::core::AlbumDownload &album);
    // Supersede (DL-7, DL-8) decided for an album that arrived.
    void superseded(const selecta::core::SupersedeReport &report);

private Q_SLOTS:
    void drain();

private:
    struct Listing
    {
        qint64 albumId = 0;
        QList<SearchHit> known;
        std::unique_ptr<QTimer> timeout;
    };
    struct PendingArrival
    {
        std::shared_ptr<AlbumArrivalJob> job;
        QList<qint64> downloads; // the album's downloads
        bool started = false;    // the worker has it (the lookups and their timeout race to start it)
    };
    struct Outcome
    {
        qint64 albumId = 0;
        QList<qint64> downloads;
        AlbumArrivalResult result;
    };

    void requestListing(const AlbumDownload &album, const QList<SearchHit> &known);
    void listed(qint64 albumId, const QList<slsk::FileEntry> &files, bool fromPeer);
    void checkComplete(qint64 albumId);
    void startArrival(AlbumDownload album);
    void runArrival(const std::shared_ptr<PendingArrival> &pending);
    void recordArrival(const Outcome &outcome);
    void supersede(const AlbumDownload &album, const library::InboxItem &item, const AlbumArrivalResult &result);
    void setState(AlbumDownload &album, AlbumDownload::State state, const QString &message);

    storage::Database &m_db;
    governor::Governor &m_governor;
    NetworkService &m_network;
    TransferService &m_transfers;
    InboxService &m_inbox;
    ActivityLog &m_activity;
    library::FileOps::TrashFunction m_trash;
    Options m_options;
    WallClock m_now;
    MetaServices *m_meta = nullptr;
    AlbumStore m_store;
    DownloadStore m_downloads;
    QHash<quint32, std::shared_ptr<Listing>> m_listings;                // folder listing token → album
    QHash<qint64, std::shared_ptr<audio::CancellationFlag>> m_arriving; // albums being made → their cancel flag
    QHash<qint64, qint64> m_supersedeTrash;                             // Inbox single sent to the Trash → its album
    QHash<qint64, QList<TrashedItem>> m_undo;                           // album → the singles its supersede trashed
    QThreadPool m_pool;
    std::mutex m_mutex;
    QList<Outcome> m_outcomes; // guarded by m_mutex
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::SupersedeReport)
