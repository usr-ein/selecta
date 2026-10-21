// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/OwnRelease.h"
#include "core/SearchResults.h"
#include "core/TransferService.h"
#include "slsk/PeerNetwork.h"
#include "slsk/Types.h"

#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace selecta::storage {
class Database;
}
namespace selecta::governor {
class Governor;
}
namespace selecta::match {
class MatchIndex;
}

namespace selecta::core {

class NetworkService;

// A peer's shares being browsed (PRD BRWS-1), as the view shows it.
struct Browse
{
    enum class State {
        Requesting, // asked; waiting for the peer
        Receiving,  // the list is arriving (received of total bytes)
        Ready,
        Failed, // message says why
    };

    QString peer;
    State state = State::Requesting;
    qint64 received = 0;
    qint64 total = 0;
    int files = 0;
    int folders = 0;
    int privateFolders = 0; // folders only the peer's buddies can download from
    bool fromCache = false; // the list was kept from an earlier browse (BRWS-2)
    qint64 fetchedAtMs = 0; // when the peer sent it, Unix time in ms
    QString message;
};

// Why a request to a peer failed, in words: "vinyl_ghost is offline.", "… didn't answer in time.".
QString describePeerError(const QString &peer, slsk::PeerError error);

// Browsing peers' shares (PRD BRWS-1, BRWS-2) on the engine thread: the whole share list is asked for through the
// governor's browse budget, kept a day in the database so looking again costs the peer nothing, and analysed like
// search results (quality, what the user owns) on a worker. Files and whole folders download from it like from a
// search: tagged from their own release, a folder as one Inbox album.
//
// Threading: engine thread; analysis on a pool of its own, back through a mutex-guarded mailbox. Ownership: every
// collaborator outlives it.
class BrowseService : public QObject
{
    Q_OBJECT

public:
    using WallClock = std::function<qint64()>;
    using Annotate = std::function<void(QList<SearchResult> &results)>;
    using Enqueue = std::function<bool(const DownloadSpec &spec)>;
    using EnqueueAlbum =
        std::function<bool(const QString &peer, const QByteArray &folder, const QList<SearchHit> &files,
                           const std::optional<meta::Release> &release)>;

    static constexpr qint64 kCacheLifetimeMs = qint64{24} * 3600 * 1000;
    static constexpr int kCachedPeers = 50;
    // What a browse lists at most: a bigger list fails, saying it's too big. Each file is analysed and kept like a
    // search result, with its whole path, so the files and the bytes their paths come to bound the memory and the work
    // one peer's list can cost (slsk caps the inflated list, not one string in it: a huge folder name times every file
    // in it would otherwise pass).
    static constexpr int kMaxFiles = 100'000;
    static constexpr qint64 kMaxPathBytes = qint64{64} * 1024 * 1024;
    // And the folders the tree may have to show: every level of every folder that holds files, counted once per
    // folder (a real list of 100,000 files has some hundred thousand; one-letter levels could make tens of millions).
    static constexpr qint64 kMaxFolderLevels = 1'000'000;

    BrowseService(storage::Database &db, governor::Governor &governor, NetworkService &network, WallClock now = {},
                  QObject *parent = nullptr);
    ~BrowseService() override;
    BrowseService(const BrowseService &) = delete;
    BrowseService &operator=(const BrowseService &) = delete;
    BrowseService(BrowseService &&) = delete;
    BrowseService &operator=(BrowseService &&) = delete;

    void setLibraryIndex(std::shared_ptr<const match::MatchIndex> index);
    void setAnnotate(Annotate annotate) { m_annotate = std::move(annotate); }
    // Annotates every open browse's results again (after the user corrected a match, LIB-6): resultsReset() for each.
    void refreshAnnotations();
    void setResolver(ResolveFunction resolve) { m_resolve = std::move(resolve); }
    void setDownloads(Enqueue enqueue, EnqueueAlbum enqueueAlbum);

    // Browses a peer: its kept list when there is a fresh one (unless `refresh`), else a request through the
    // governor, which may refuse (too many browses, the same peer lately): browseChanged() says why.
    void browse(const QString &peer, bool refresh = false);
    void close(const QString &peer);
    std::optional<Browse> browseOf(const QString &peer) const;
    QList<SearchResult> results(const QString &peer) const { return m_results.value(peer); }
    // Downloads these results of a browse into the Inbox (DL-3); the folder of one, whole, as an album (DL-1).
    int download(const QString &peer, const QList<int> &indexes);
    bool downloadFolder(const QString &peer, int index);
    // The kept lists, and forgetting them (CACHE-2).
    int cachedPeers();
    void clearCache();

    // --- fed by the network ---
    void listReceived(const QString &peer, const slsk::SharedFileList &list);
    void listProgress(const QString &peer, qint64 received, qint64 total);
    void requestFailed(const QString &peer, const QString &why);
    // Blocks until the analysis in flight is done and recorded. For tests.
    void waitForIdle();

Q_SIGNALS:
    void browseChanged(const selecta::core::Browse &browse);
    void browseClosed(const QString &peer);
    void resultsReset(const QString &peer, const QList<selecta::core::SearchResult> &results);

private Q_SLOTS:
    void drain();

private:
    struct Analysed
    {
        QString peer;
        quint64 generation = 0;
        QList<SearchResult> results;
    };

    void accept(const QString &peer, const slsk::SharedFileList &list, qint64 fetchedAtMs, bool fromCache);
    // Why this list is too big to show (kMaxFiles, kMaxPathBytes), or nothing.
    static std::optional<QString> tooBig(const QString &peer, const slsk::SharedFileList &list);
    void store(const QString &peer, const slsk::SharedFileList &list);
    std::optional<std::pair<slsk::SharedFileList, qint64>> cached(const QString &peer);
    void setState(Browse &browse, Browse::State state, const QString &message = {});

    storage::Database &m_db;
    governor::Governor &m_governor;
    NetworkService &m_network;
    WallClock m_now;
    Annotate m_annotate;
    ResolveFunction m_resolve;
    Enqueue m_enqueue;
    EnqueueAlbum m_enqueueAlbum;
    std::shared_ptr<const match::MatchIndex> m_index;
    QHash<QString, Browse> m_browses;              // by peer
    QHash<QString, QList<SearchResult>> m_results; // by peer
    QHash<QString, quint64> m_generation;          // by peer: older analyses are dropped
    QThreadPool m_pool;
    std::mutex m_mutex;
    QList<Analysed> m_mailbox; // guarded by m_mutex
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::Browse)
