// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ActivityLog.h"
#include "core/AlbumService.h"
#include "core/ArrivalService.h"
#include "core/ClipPreviews.h"
#include "core/CollectionService.h"
#include "core/GetTracksService.h"
#include "core/InboxService.h"
#include "core/ManualSearchService.h"
#include "core/MetaServices.h"
#include "core/NetworkService.h"
#include "core/SearchCache.h"
#include "core/SearchService.h"
#include "core/ShareService.h"
#include "core/TransferService.h"
#include "core/YouTubePreviews.h"
#include "governor/Governor.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QTimer;

namespace selecta::base {
class Clock;
}
namespace selecta::storage {
class Database;
}

namespace selecta::core {

class GovernorStore;

struct EngineOptions
{
    QString databasePath;               // the engine opens its own connection; ":memory:" in tests
    bool startPaused = false;           // the user's "Pause network activity" from the last run (SAFE-5)
    const base::Clock *clock = nullptr; // default: a steady clock owned by the engine
    std::function<qint64()> wallClock;  // Unix time, ms; default: the system clock
    governor::Governor::Options governor;
    LibraryConfig library;
    NetworkConfig network;
    library::FileOps::TrashFunction trash; // default: the macOS Trash; tests inject their own
    // Where secrets live: the app passes the Keychain; default: an in-memory store (tests).
    std::function<std::unique_ptr<meta::TokenStore>()> tokenStore;
    // Opens a URL in the browser, for the Spotify sign-in; called on the engine thread.
    std::function<bool(const QUrl &)> openUrl;
    MetaEndpoints metaEndpoints; // empty: the real services
    // Downloads: partial and complete originals (incoming), and converted files before they are published (staging).
    // Empty: downloads can't be stored (tests that don't download).
    QString incomingDir;
    QString stagingDir;
    QString previewDir;          // previews, emptied at start
    bool watchCollection = true; // follow changes in the collection folders (tests turn it off)
    std::optional<int> session;  // this run's session number, recorded with downloads
    QString ytDlpPath;           // Settings → Previews; empty: the usual places
};

// Where a preview's audio comes from (PRD PREV-12, D26).
enum class PreviewSource : quint8 {
    Automatic, // the user's yt-dlp, else Deezer's 30-second clip, else the peer's file
    YouTube,
    Clip,
    Peer, // the very file a Get would download
};

// What a preview belongs to, as previewStarted() and previewRefused() name it: a Get tracks line or a search result.
inline QString previewKeyForLine(qint64 lineId)
{
    return QStringLiteral("line:%1").arg(lineId);
}
inline QString previewKeyForResult(qint64 searchId, int index)
{
    return QStringLiteral("search:%1:%2").arg(searchId).arg(index);
}

// The services on the engine thread (PLAN §2.3, the network thread): the governor and its saved state and the
// activity log, then the Soulseek client, metadata services, searches, transfers and the arrival pipeline as they land.
//
// Threading: created on the UI thread and moved to the engine thread by EngineHost before start(); from then on every
// method runs on the engine thread. Other threads talk to it only through EngineHost.
// Ownership: owns every service; services are created in start() so they live on the engine thread.
class Engine : public QObject
{
    Q_OBJECT

public:
    explicit Engine(EngineOptions options, QObject *parent = nullptr);
    ~Engine() override;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) = delete;
    Engine &operator=(Engine &&) = delete;

    // Opens the database connection, brings the schema up to date and restores saved state, then emits
    // startFinished(). Returns false, with error() set, if the engine can't run.
    bool start();
    // Stops every service and closes the database. Idempotent; the destructor calls it.
    void stop();
    bool isStarted() const { return m_started; }
    QString error() const { return m_error; }

    // Get tracks: parses pasted text into lines and imports any Spotify track, album or playlist links in it
    // (BATCH-1, META-3). tracksAdded() reports each part as it completes.
    void addTracks(const QString &text);

    // Previews a Get tracks line (PREV-1, PREV-12): previewStarted() hands over the source to play while the audio
    // arrives, or previewRefused() says why nothing can play. Automatic tries the user's yt-dlp, then Deezer's clip,
    // then the peer's file. The preview playing before stops.
    void previewLine(qint64 lineId, PreviewSource source = PreviewSource::Automatic);
    // Previews a search result likewise: its release (or the track its name says), or its very file from the peer.
    void previewResult(qint64 searchId, int index, PreviewSource source = PreviewSource::Automatic);
    void stopPreview();

    // Searches typed by hand (SRCH-1): opens one, or finds the open search for the same text; searchOpened() answers
    // `request` with its id.
    void openSearch(quint64 request, const QString &text);

    // Settings → Previews: the yt-dlp to run (empty: the usual places), and a check of what will run, reported by
    // ytDlpChecked().
    void setYtDlpPath(const QString &path);
    void checkYtDlp();

    // The Inbox inspector (DL-4, META-2, META-4). Edits write the user's tags over a single's own; another release
    // re-tags it from that release (label and genre from MusicBrainz, its cover), as an arrival would.
    // inboxOperationFinished() reports both.
    void editInboxTags(qint64 itemId, const convert::MetadataModel &changes, const QStringList &cleared);
    void findInboxReleases(qint64 itemId);
    void applyInboxRelease(qint64 itemId, const meta::Release &release);

    // Settings → Library: the Inbox and the collection folders (the collection is scanned again when they change).
    void setLibraryConfig(const LibraryConfig &config);

    // "Pause network activity" (SAFE-5): no new Soulseek operations until resumed.
    void setPaused(bool paused);
    bool isPaused() const;

    // For services and tests on the engine thread. Valid between start() and stop().
    governor::Governor &governor() { return *m_governor; }
    ActivityLog &activity() { return *m_activity; }
    InboxService &inbox() { return *m_inbox; }
    MetaServices &meta() { return *m_meta; }
    GetTracksService &getTracks() { return *m_getTracks; }
    NetworkService &network() { return *m_network; }
    SearchService &search() { return *m_search; }
    ManualSearchService &manualSearch() { return *m_manualSearch; }
    SearchCacheStore &searchCache() { return *m_searchCache; }
    TransferService &transfers() { return *m_transfers; }
    ArrivalService &arrival() { return *m_arrival; }
    AlbumService &albums() { return *m_albums; }
    CollectionService &collection() { return *m_collection; }
    ShareService &shares() { return *m_shares; }
    storage::Database &database() { return *m_db; }

Q_SIGNALS:
    void startFinished(bool ok, const QString &error);
    void pausedChanged(bool paused);
    void searchBudgetChanged(int available);
    void activityRecorded(const selecta::core::ActivityEntry &entry);
    void inboxChanged(const QList<selecta::core::InboxEntry> &entries);
    void inboxOperationFinished(qint64 id, bool ok, const QString &message);
    void spotifyStatusChanged(const selecta::core::SpotifyStatus &status);
    void getLinesReset(const QList<selecta::core::GetLine> &lines);
    void getLineChanged(const selecta::core::GetLine &line);
    // Lines added, already there, unidentified, and a sentence about anything else worth saying.
    void tracksAdded(int added, int duplicates, int unidentified, const QString &note);
    void networkStatusChanged(const selecta::core::NetworkStatus &status);
    // Searches waiting for the governor, collecting results, and the wait before the next one.
    void searchQueueChanged(int waiting, int collecting, qint64 nextInMs);
    // A preview started: the source to play (it grows while the file arrives), a stable identity for the analysis
    // cache, and what to call it.
    void previewStarted(const QString &key, qint64 previewId, const std::shared_ptr<selecta::audio::ByteSource> &source,
                        const QString &identity, const QString &title, const QString &artist, const QString &label);
    void previewRefused(const QString &key, const QString &why);
    void previewChanged(const selecta::core::Preview &preview);
    void collectionStatusChanged(const selecta::core::CollectionStatus &status);
    void shareStatusChanged(const selecta::core::ShareStatus &status);
    void uploadsChanged(const QList<selecta::core::UploadEntry> &uploads);
    // Searches typed by hand (ManualSearchService's signals, forwarded).
    void searchOpened(quint64 request, qint64 searchId);
    void manualSearchChanged(const selecta::core::ManualSearch &search);
    void manualSearchClosed(qint64 searchId);
    void searchResultsAdded(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    void searchResultsReset(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    void searchResultsChanged(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    // What checkYtDlp() found: whether previews can come from YouTube, and in words what or why.
    void ytDlpChecked(bool ok, const QString &description);
    // Release candidates for an Inbox item (findInboxReleases()), or why there are none.
    void inboxReleasesFound(qint64 itemId, const QList<selecta::meta::Candidate> &candidates, const QString &error);
    // Album downloads (AlbumService's signals, forwarded).
    void albumChanged(const selecta::core::AlbumDownload &album);
    void superseded(const selecta::core::SupersedeReport &report);

private:
    // What a preview can play from: the release (YouTube, the clip) and the peer's file.
    struct PreviewPlan
    {
        QString key;
        std::optional<meta::Release> release;
        std::optional<DownloadSpec> peer;
        QString title;
        QString artist;
    };
    void startPreview(PreviewPlan plan, PreviewSource source);
    void tryPreview(const std::shared_ptr<const PreviewPlan> &plan, const QList<PreviewSource> &attempts,
                    qsizetype next, quint64 generation, QString lastError);
    bool fail(const QString &error);
    void pruneActivity();
    // Reports the search budget now and again whenever it refills, until it's full.
    void reportSearchBudget();
    // What only the engine knows about search results: queued downloads, this session's, owned files, peer counters.
    void annotate(QList<SearchResult> &results);

    EngineOptions m_options;
    std::unique_ptr<base::Clock> m_ownClock;
    const base::Clock *m_clock = nullptr;
    std::unique_ptr<storage::Database> m_db;
    std::unique_ptr<governor::Governor> m_governor;
    std::unique_ptr<GovernorStore> m_governorStore;
    std::unique_ptr<ActivityLog> m_activity;
    std::unique_ptr<InboxService> m_inbox;
    std::unique_ptr<MetaServices> m_meta;
    std::unique_ptr<GetTracksService> m_getTracks;
    std::unique_ptr<NetworkService> m_network;
    std::unique_ptr<SearchNetwork> m_searchNetwork;
    std::unique_ptr<SearchCacheStore> m_searchCache;
    std::unique_ptr<SearchService> m_search;
    std::unique_ptr<ManualSearchService> m_manualSearch;
    std::unique_ptr<TransferService> m_transfers;
    std::unique_ptr<ArrivalService> m_arrival;
    std::unique_ptr<AlbumService> m_albums;
    std::unique_ptr<CollectionService> m_collection;
    std::unique_ptr<ShareService> m_shares;
    std::unique_ptr<YouTubePreviews> m_youtube;
    std::unique_ptr<ClipPreviews> m_clips;
    qint64 m_previewId = 0;          // the preview playing
    quint64 m_previewGeneration = 0; // bumps when a preview is stopped or replaced
    QTimer *m_pruneTimer = nullptr;
    QTimer *m_budgetTimer = nullptr;
    QTimer *m_annotateTimer = nullptr; // coalesces transfer changes before search results are annotated again
    bool m_started = false;
    QString m_error;
};

} // namespace selecta::core
