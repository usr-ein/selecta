// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ActivityLog.h"
#include "core/Engine.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <optional>

namespace selecta::core {

// Runs the Engine on its own thread and is the UI's only way to reach it. Lives on the UI thread.
//
// Calls are posted to the engine thread and return at once; results come back as this object's signals, delivered on
// the UI thread. State the UI shows (paused, search budget) is cached here from those signals, so reading it never
// blocks. stop() and the destructor wait for the engine thread to finish.
class EngineHost : public QObject
{
    Q_OBJECT

public:
    explicit EngineHost(EngineOptions options, QObject *parent = nullptr);
    ~EngineHost() override;
    EngineHost(const EngineHost &) = delete;
    EngineHost &operator=(const EngineHost &) = delete;
    EngineHost(EngineHost &&) = delete;
    EngineHost &operator=(EngineHost &&) = delete;

    // Starts the engine thread; started() reports whether the engine came up. Does nothing if already running.
    void start();
    // Stops the engine and joins its thread. Blocks until both are done; safe to call more than once.
    void stop();
    bool isRunning() const { return m_thread.isRunning(); }

    bool isPaused() const { return m_paused; }
    int searchesAvailable() const { return m_searchesAvailable; }

    void setPaused(bool paused);

    // --- the Inbox ---
    void setLibraryConfig(const LibraryConfig &config);
    void refreshInbox();
    void fileInboxItems(const QList<qint64> &ids);
    // Moves Inbox items to the Trash; each one that goes is reported (inboxItemTrashed()) for Undo.
    void trashInboxItems(const QList<qint64> &ids);
    void restoreInboxItems(const QList<selecta::core::TrashedItem> &items);
    QList<InboxEntry> inboxEntries() const { return m_inboxEntries; }

    // --- Spotify (Settings → Spotify) ---
    void setSpotifyCredentials(const QString &clientId, const QByteArray &secret, bool keepSavedSecret);
    void signInToSpotify();
    void signOutOfSpotify();
    SpotifyStatus spotifyStatus() const { return m_spotifyStatus; }

    // --- Get tracks ---
    void addTracks(const QString &text);
    void removeTracks(const QList<qint64> &ids);
    void clearTracks();
    void tickTracks(const QList<qint64> &ids, bool ticked);
    void retryTracks(const QList<qint64> &ids);
    // Finds the best file for these lines (all lines still to find when empty), paced by the governor.
    void findTracks(const QList<qint64> &ids = {});
    void stopFindingTracks();
    // Downloads the chosen file of these lines (every ticked line when empty) into the Inbox, converted and tagged.
    void downloadTracks(const QList<qint64> &ids = {});
    void cancelTrackDownloads(const QList<qint64> &ids);
    // Previews a line's chosen file while it arrives (previewStarted / previewRefused); stops the one playing.
    void previewTrack(qint64 lineId, PreviewSource source = PreviewSource::Automatic);
    void stopPreview();

    // --- previews (PREV-11, PREV-12) ---
    void setYtDlpPath(const QString &path);
    void checkYtDlp();

    // --- the Inbox inspector (DL-4, META-2, META-4) ---
    void editInboxTags(qint64 itemId, const convert::MetadataModel &changes, const QStringList &cleared);
    void findInboxReleases(qint64 itemId);
    void applyInboxRelease(qint64 itemId, const meta::Release &release);

    // --- searches typed by hand (SRCH-1) ---
    // Opens a search, or finds the open one with the same text: searchOpened() answers with the returned request
    // number.
    quint64 openSearch(const QString &text);
    void closeSearch(qint64 searchId);
    // Ranks the results against another release candidate (-1: "Any (name only)").
    void chooseSearchRelease(qint64 searchId, int candidate);
    void refreshSearch(qint64 searchId);
    void widenSearch(qint64 searchId);
    void downloadResults(qint64 searchId, const QList<int> &indexes);
    // Downloads the remote folder of this result whole, as one Inbox album (DL-1, DL-12).
    void downloadFolder(qint64 searchId, int index);
    void previewResult(qint64 searchId, int index, PreviewSource source = PreviewSource::Automatic);
    // Open searches and their results, as last reported.
    QList<ManualSearch> searches() const { return m_searches; }
    std::optional<ManualSearch> search(qint64 searchId) const;
    QList<SearchResult> searchResults(qint64 searchId) const { return m_searchResults.value(searchId); }

    // --- the collection ---
    void scanCollection();
    CollectionStatus collectionStatus() const { return m_collectionStatus; }
    // --- transfers (DL-1) and peers (FAIR-1) ---
    // This session's downloads and the unfinished ones, by id; bytes on disk and file size while they arrive.
    QList<Download> downloads() const { return m_downloads.values(); }
    std::optional<Download> download(qint64 id) const;
    std::pair<qint64, qint64> progressOf(qint64 id) const { return m_progress.value(id, {-1, -1}); }
    void cancelDownloads(const QList<qint64> &ids);
    void retryDownloads(const QList<qint64> &ids);
    QList<PeerStats> peerStats() const { return m_peerStats; }
    void refreshPeerStats();

    // --- the converter (CONV-1…7) ---
    void addToConverter(const QStringList &paths);
    void removeFromConverter(const QList<qint64> &ids);
    void clearFinishedConversions();
    void setConverterOptions(const ConverterOptions &options);
    void confirmConversion(qint64 id, bool yes);
    // Converts these items (every convertible one when empty).
    void startConversions(const QList<qint64> &ids = {});
    void cancelConversions();
    QList<ConverterItem> converterItems() const { return m_converterItems; }

    // --- album downloads (DL-1, DL-3, DL-7, DL-8) ---
    void cancelAlbum(qint64 albumId);
    // Puts back the Inbox singles an album sent to the Trash.
    void undoSupersede(qint64 albumId);
    // Moves to the Trash the singles an album held back to ask first (AlbumReport::held).
    void confirmSupersede(qint64 albumId);
    // Album downloads of this run, and unfinished ones, as last reported.
    QList<AlbumDownload> albums() const { return m_albums.values(); }

    // --- sharing (SHARE-1…5, FAIR-3) ---
    ShareStatus shareStatus() const { return m_shareStatus; }
    QList<UploadEntry> uploads() const { return m_uploads; }
    QList<GetLine> getLines() const { return m_getLines; }

    // --- the Soulseek network (Settings → Network) ---
    void setNetworkConfig(const NetworkConfig &config);
    void setNetworkPassword(const QString &password);
    void connectNetwork();
    void disconnectNetwork();
    NetworkStatus networkStatus() const { return m_networkStatus; }

    // Runs `work(engine)` on the engine thread, if the engine is running. For services that have no dedicated
    // method here yet; `work` must not capture anything owned by the UI thread by reference.
    template <typename Work>
    void post(Work work)
    {
        if (m_engine) {
            QMetaObject::invokeMethod(
                m_engine.data(), [engine = m_engine.data(), work = std::move(work)] { work(*engine); },
                Qt::QueuedConnection);
        }
    }

Q_SIGNALS:
    void started(bool ok, const QString &error);
    void pausedChanged(bool paused);
    void searchBudgetChanged(int available);
    void activityRecorded(const selecta::core::ActivityEntry &entry);
    void inboxChanged(const QList<selecta::core::InboxEntry> &entries);
    void inboxOperationFinished(qint64 id, bool ok, const QString &message);
    void inboxItemTrashed(const selecta::core::TrashedItem &item);
    void spotifyStatusChanged(const selecta::core::SpotifyStatus &status);
    void getLinesReset(const QList<selecta::core::GetLine> &lines);
    void getLineChanged(const selecta::core::GetLine &line);
    // What adding did, for a short message: lines added, already there, unidentified, and a note.
    void tracksAdded(int added, int duplicates, int unidentified, const QString &note);
    void networkStatusChanged(const selecta::core::NetworkStatus &status);
    void searchQueueChanged(int waiting, int collecting, qint64 nextInMs);
    // `key` names what the preview belongs to (previewKeyForLine(), previewKeyForResult()).
    void previewStarted(const QString &key, qint64 previewId, const std::shared_ptr<selecta::audio::ByteSource> &source,
                        const QString &identity, const QString &title, const QString &artist, const QString &label);
    void previewRefused(const QString &key, const QString &why);
    void previewChanged(const selecta::core::Preview &preview);
    void collectionStatusChanged(const selecta::core::CollectionStatus &status);
    void shareStatusChanged(const selecta::core::ShareStatus &status);
    void uploadsChanged(const QList<selecta::core::UploadEntry> &uploads);
    void inboxReleasesFound(qint64 itemId, const QList<selecta::meta::Candidate> &candidates, const QString &error);
    void ytDlpChecked(bool ok, const QString &description);
    void searchOpened(quint64 request, qint64 searchId);
    void manualSearchChanged(const selecta::core::ManualSearch &search);
    void manualSearchClosed(qint64 searchId);
    void searchResultsAdded(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    void searchResultsReset(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    void searchResultsChanged(qint64 searchId, const QList<selecta::core::SearchResult> &results);
    void albumChanged(const selecta::core::AlbumDownload &album);
    void downloadsReset();
    void downloadChanged(const selecta::core::Download &download);
    void downloadProgress(qint64 id, qint64 bytesDone, qint64 bytesTotal);
    void peerStatsChanged(const QList<selecta::core::PeerStats> &peers);
    void converterChanged(const QList<selecta::core::ConverterItem> &items);
    void conversionsFinished(int done, int failed);
    void albumReported(const selecta::core::AlbumReport &report);

private:
    EngineOptions m_options;
    QThread m_thread;
    QPointer<Engine> m_engine;
    bool m_paused = false;
    int m_searchesAvailable = 0;
    QList<InboxEntry> m_inboxEntries;
    SpotifyStatus m_spotifyStatus;
    QList<GetLine> m_getLines;
    NetworkStatus m_networkStatus;
    CollectionStatus m_collectionStatus;
    ShareStatus m_shareStatus;
    QList<UploadEntry> m_uploads;
    QMap<qint64, AlbumDownload> m_albums;                // by id: oldest first
    QMap<qint64, Download> m_downloads;                  // by id: oldest first
    QHash<qint64, std::pair<qint64, qint64>> m_progress; // bytes on disk, file size
    QList<PeerStats> m_peerStats;
    QList<ConverterItem> m_converterItems;
    QList<ManualSearch> m_searches; // in opening order
    QHash<qint64, QList<SearchResult>> m_searchResults;
    quint64 m_nextSearchRequest = 1;
};

} // namespace selecta::core
