// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Downloads.h"
#include "core/FileRanking.h"
#include "core/GetTracks.h"
#include "meta/ReleaseResolver.h"

#include <QFuture>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

class SearchService;
class TransferService;

// Get tracks (PRD D27, BATCH-1…9) on the engine thread: the list the user pasted, each line matched to a release
// before any Soulseek search (BATCH-5), then found, previewed and downloaded. The lines live in the database, so the
// list survives a restart; a line resolved once stays resolved.
//
// Threading: engine thread. Resolutions run asynchronously, at most kMaxResolving at a time; their results come back
// on this thread. Changes are published as lineChanged() for one line and linesReset() when lines come or go.
class GetTracksService : public QObject
{
    Q_OBJECT

public:
    // How a line becomes release candidates: meta's ReleaseResolver in the app, a fake in tests.
    using ResolveFunction = std::function<QFuture<meta::Result<meta::Resolution>>(const meta::ResolveQuery &)>;

    static constexpr int kMaxResolving = 3;

    struct AddResult
    {
        int added = 0;
        int duplicates = 0;   // already in the list, or twice in what was pasted
        int unidentified = 0; // "ID - ID" and the like: never searched (note §9.1)
    };

    explicit GetTracksService(storage::Database &db, std::function<qint64()> wallClock = {}, QObject *parent = nullptr);
    ~GetTracksService() override;
    GetTracksService(const GetTracksService &) = delete;
    GetTracksService &operator=(const GetTracksService &) = delete;
    GetTracksService(GetTracksService &&) = delete;
    GetTracksService &operator=(GetTracksService &&) = delete;

    // Sets how releases are found and starts resolving pending lines; an empty function pauses resolution.
    void setResolver(ResolveFunction resolve);

    // Parses pasted text (one track per line) and appends the new lines (BATCH-1).
    AddResult add(const QString &text);
    // Appends tracks that already carry their release (a Spotify playlist or album, META-3).
    AddResult addReleases(const QList<meta::Release> &tracks);
    void remove(const QList<qint64> &ids);
    void clear();
    void setTicked(const QList<qint64> &ids, bool ticked);
    // Resolves these lines again (after a failure, or to look for a better release).
    void retry(const QList<qint64> &ids);
    // The user picked a release among the candidates.
    void chooseRelease(qint64 id, const meta::Release &release);

    // --- finding the files (BATCH-4, BATCH-5) ---
    // Where searches go, and the server's excluded phrases (for the query planner). Not owned.
    void setSearch(SearchService *search, std::function<QStringList()> excludedPhrases);
    void setQualityWanted(QualityWanted wanted) { m_wanted = wanted; }
    QualityWanted qualityWanted() const { return m_wanted; }
    // Queues a search for each of these lines (every line still to find when empty). Lines still waiting for their
    // release lookup are searched once it is done.
    void find(const QList<qint64> &ids = {});
    // Stops searching: queued searches are dropped and their lines go back to idle.
    void stopFinding();
    bool isFinding() const { return !m_finding.isEmpty() || !m_findAfterLookup.isEmpty(); }
    int findingCount() const { return static_cast<int>(m_finding.size() + m_findAfterLookup.size()); }

    // --- downloading (D27) ---
    // Where downloads go. Not owned.
    void setTransfers(TransferService *transfers) { m_transfers = transfers; }
    // Downloads the chosen file of these lines (every ticked line when empty). Lines already downloading or arrived
    // are left alone; a failed line tries again.
    void download(const QList<qint64> &ids = {});
    // Stops these lines' downloads.
    void cancelDownloads(const QList<qint64> &ids);
    // From the TransferService: a download's state, and its progress while it transfers. A download that fails for
    // good moves on to the next peer's copy when the search found one.
    void downloadChanged(const selecta::core::Download &download);
    void downloadProgress(qint64 downloadId, qint64 bytesDone, qint64 bytesTotal);
    // After the TransferService restored its queue: lines a quit left downloading without a download are reset.
    void downloadsRestored(const QList<selecta::core::Download> &unfinished);

    QList<GetLine> lines() const { return m_lines; }
    int resolvingCount() const { return static_cast<int>(m_resolving.size()); }

Q_SIGNALS:
    void linesReset(const QList<selecta::core::GetLine> &lines);
    void lineChanged(const selecta::core::GetLine &line);

private:
    void load();
    void startFinding(GetLine &line);
    void found(qint64 id, const SearchOutcome &outcome);
    void resolveNext();
    void resolved(qint64 id, const meta::Result<meta::Resolution> &result);
    void store(const GetLine &line);
    GetLine *lineWithId(qint64 id);
    void startDownload(GetLine &line);
    bool tryAlternative(GetLine &line, const QString &reason);
    void cancelDownloadOf(const GetLine &line);

    storage::Database &m_db;
    std::function<qint64()> m_now;
    GetTracksStore m_store;
    ResolveFunction m_resolve;
    QList<GetLine> m_lines;                                             // in list order, mirrors the database
    QHash<qint64, QFuture<meta::Result<meta::Resolution>>> m_resolving; // line id → resolution; cancelled if it goes
    SearchService *m_search = nullptr;
    std::function<QStringList()> m_excludedPhrases;
    QualityWanted m_wanted = QualityWanted::Mp3_320OrLossless;
    QHash<qint64, quint64> m_finding; // line id → its queued or running search
    QSet<qint64> m_findAfterLookup;   // lines to search once their release lookup ends
    TransferService *m_transfers = nullptr;
};

} // namespace selecta::core
