// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "collection/FolderRoles.h"
#include "governor/TokenBucket.h"
#include "share/ShareIndex.h"
#include "share/UploadQueue.h"
#include "slsk/Client.h"
#include "slsk/Providers.h"
#include "slsk/TransferManager.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QIODevice;
class QTimer;

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}

namespace selecta::core {

class ActivityLog;
class NetworkService;

// Files and bytes, for the ratio.
struct TransferTotals
{
    int files = 0;
    qint64 bytes = 0;

    friend bool operator==(const TransferTotals &, const TransferTotals &) = default;
};

// What the Shares view and the toolbar show (PRD SHARE-1, SHARE-5, FAIR-3).
struct ShareStatus
{
    QStringList roots; // the shared folders
    int folders = 0;   // what peers see
    int files = 0;
    bool building = false;
    QString message; // a shared folder that isn't there, files that can't be shared…
    int uploading = 0;
    int queued = 0;
    int searchesAnswered = 0; // this session
    int browsed = 0;          // share lists sent this session
    TransferTotals uploadedSession;
    TransferTotals uploaded30Days;
    TransferTotals uploadedAllTime;
    TransferTotals downloadedSession;
    TransferTotals downloaded30Days;
    TransferTotals downloadedAllTime;
};

// One upload, as the Shares view lists it.
struct UploadEntry
{
    slsk::TransferId transfer = 0; // 0 while queued
    QString user;
    QString path; // the virtual path, as the peer sees it
    quint64 size = 0;
    quint64 sent = 0;
    bool active = false;
};

// Sharing (PRD SHARE-1…3, SHARE-5, D12) on the engine thread: the collection is shared, read-only. The share index is
// built on a worker from the collection folders (the collection scan's audio facts reused) and answers browses,
// folder listings and searches (share::ShareIndex); uploads go in the order of share::UploadQueue: three slots, one
// per user, privileged users first, users taking turns. Every upload that ends is recorded for the ratio.
//
// Answers cost us an outbound connection each, under the governor's peer-lookup budget; on top of that Selecta answers
// at most kAnswersPerMinute searches, so answering never crowds out what the user asked for. "Pause network activity"
// (SAFE-5) stops answering and starting uploads; uploads under way finish.
//
// Threading: engine thread; builds run on a worker and come back through a mutex-guarded mailbox. Ownership: the
// database, governor, network service and activity log outlive it. It registers itself as the network's share and
// user-info provider and unregisters in the destructor.
class ShareService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;

    static constexpr int kAnswersBurst = 5;
    static constexpr int kAnswersPerMinute = 10;
    static constexpr int kRebuildDelayMs = 2000; // coalesces collection changes

    ShareService(storage::Database &db, governor::Governor &governor, NetworkService &network, ActivityLog &activity,
                 const base::Clock &clock, std::optional<qint64> session, WallClock now = {},
                 QObject *parent = nullptr);
    ~ShareService() override;
    ShareService(const ShareService &) = delete;
    ShareService &operator=(const ShareService &) = delete;
    ShareService(ShareService &&) = delete;
    ShareService &operator=(ShareService &&) = delete;

    // The shared folders: the collection's two (D12), each by its role, so peers get exactly the collection's tracks
    // (SHARE-1): `<tracks folder>\<track>` and `<albums folder>\<album>\<track>`, nothing else (share::build()).
    // `excluded`: Selecta's own folders (its Inbox, caches), never shared even when they lie in one. Builds again.
    void setFolders(const collection::FolderRoles &folders, const QStringList &excluded);
    // The collection was scanned again: the index is built again from it, shortly.
    void collectionChanged();
    // A download arrived: the ratio moves.
    void refreshTotals();
    // Banned users (NET-8): their uploads are denied with "Banned", and they see nothing of our shares.
    void setBanned(const QSet<QString> &banned);

    ShareStatus status() const;
    QList<UploadEntry> uploads() const;
    std::shared_ptr<const share::ShareIndex> index() const { return m_index; }
    // Blocks until a build in flight is done and recorded. For tests.
    void waitForIdle();

    // --- what peers ask (through the network's ShareProvider and UserInfoProvider): synchronous, engine thread ---
    QByteArray sharedFileList(const QString &username);
    QList<slsk::SharedDirectory> folderContents(const QString &username, const slsk::RemotePath &folder);
    slsk::peer::UserInfoResponse userInfo(const QString &username);

Q_SIGNALS:
    void statusChanged(const selecta::core::ShareStatus &status);
    void uploadsChanged(const QList<selecta::core::UploadEntry> &uploads);

private Q_SLOTS:
    void drain();

private:
    struct Built
    {
        share::BuildResult result;
        quint64 generation = 0;
    };
    // The network asks these; they ask the service.
    class Shares final : public slsk::ShareProvider
    {
    public:
        explicit Shares(ShareService &owner)
            : m_owner(owner)
        {}
        QByteArray sharedFileList(const QString &username) override { return m_owner.sharedFileList(username); }
        QList<slsk::SharedDirectory> folderContents(const QString &username, const slsk::RemotePath &folder) override
        {
            return m_owner.folderContents(username, folder);
        }

    private:
        ShareService &m_owner;
    };
    class Info final : public slsk::UserInfoProvider
    {
    public:
        explicit Info(ShareService &owner)
            : m_owner(owner)
        {}
        slsk::peer::UserInfoResponse userInfo(const QString &username) override { return m_owner.userInfo(username); }

    private:
        ShareService &m_owner;
    };
    struct Active
    {
        std::unique_ptr<QIODevice> device;
        QString user;
        slsk::RemotePath path;
        quint64 size = 0;
        quint64 sent = 0;
        qint64 startedMs = 0;
    };

    void rebuild();
    void searchReceived(const slsk::IncomingSearch &search);
    void uploadRequested(const QString &user, const slsk::RemotePath &path, bool legacy);
    void placeInQueueRequested(const QString &user, const slsk::RemotePath &path);
    void uploadChanged(const slsk::UploadInfo &info);
    void uploadProgress(slsk::TransferId id, quint64 sent, quint64 total);
    void startUploads();
    void record(const Active &upload, const slsk::UploadInfo &info);
    bool isPaused() const;
    slsk::Client::UploadAvailability availability() const;
    void publish();
    void publishUploads();

    storage::Database &m_db;
    governor::Governor &m_governor;
    NetworkService &m_network;
    ActivityLog &m_activity;
    std::optional<qint64> m_session;
    WallClock m_now;
    QList<share::ShareRoot> m_roots;
    QStringList m_excluded;
    std::shared_ptr<const share::ShareIndex> m_index;
    share::UploadQueue m_queue;
    QHash<slsk::TransferId, std::shared_ptr<Active>> m_active;
    QSet<QString> m_privileged;
    governor::TokenBucket m_answers;
    ShareStatus m_status;
    Shares m_shares{*this};
    Info m_info{*this};
    QSet<QString> m_banned;
    QTimer *m_rebuildTimer = nullptr;
    QTimer *m_tick = nullptr; // tries the queue again now and then, and keeps the totals fresh
    QThreadPool m_pool;
    std::shared_ptr<audio::CancellationFlag> m_cancel; // the build in flight
    quint64 m_generation = 0;
    bool m_building = false;
    bool m_rebuildWanted = false;
    std::mutex m_mutex;
    QList<Built> m_built; // guarded by m_mutex
};

// How the ratio looks (FAIR-3), for the toolbar and the Shares view: over the last 30 days, bytes given back against
// bytes taken. Healthy while little has been taken, or a quarter of it has gone back; low otherwise; not sharing at
// all when nothing is shared. Pure.
struct RatioAssessment
{
    enum class Level : quint8 {
        Healthy,
        Low,
        NotSharing,
    };
    Level level = Level::Healthy;
    QString summary;  // "↑ 1.2 GB · ↓ 3.4 GB"
    QString guidance; // what to do about it, in words
};
RatioAssessment assessRatio(const ShareStatus &status);

// Uploads and downloads that ended since `sinceMs` (Unix time, ms; 0: all time), or in `session` when given: the
// ratio's two sides (FAIR-3). Engine thread (it reads the database).
TransferTotals uploadTotals(storage::Database &db, qint64 sinceMs, std::optional<qint64> session = std::nullopt);
TransferTotals downloadTotals(storage::Database &db, qint64 sinceMs, std::optional<qint64> session = std::nullopt);

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ShareStatus)
Q_DECLARE_METATYPE(selecta::core::UploadEntry)
