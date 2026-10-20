// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchResults.h"
#include "core/SearchService.h"
#include "core/TransferService.h"
#include "match/MatchIndex.h"
#include "meta/ReleaseResolver.h"

#include <QFuture>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

class QTimer;

namespace selecta::core {

// A search typed by hand (PRD SRCH-1, SRCH-17…19), as the sidebar and the Search view show it.
struct ManualSearch
{
    enum class State : quint8 {
        LookingUp, // finding the release first (SRCH-17)
        Waiting,   // queued behind the governor's budget
        Searching, // results are coming in
        Done,
        Failed, // nothing could be sent: `message` says why
    };

    qint64 id = 0;
    QString text; // as typed
    State state = State::LookingUp;
    QList<meta::Candidate> candidates; // release candidates, best first
    int chosen = -1;                   // the release the results are ranked against; -1: "Any (name only)"
    bool chosenAutomatically = false;  // the text names this track (SRCH-17)
    QString query;                     // as sent to the network
    bool respelled = false;            // sent spelled as the release, not as typed (SRCH-19)
    bool canWiden = false;             // "Search wider" has a query left to try
    int files = 0;                     // results so far
    int peers = 0;
    bool fromCache = false;
    qint64 searchedAtMs = 0; // cached results: when the search ran
    QString message;         // why it failed, or a note worth saying

    const meta::Release *chosenRelease() const
    {
        return chosen >= 0 && chosen < candidates.size() ? &candidates.at(chosen).release : nullptr;
    }
};

// Searches typed by hand, on the engine thread (PRD SRCH-1, SRCH-11, SRCH-12, SRCH-17…19). Each search first looks up
// the release the text names, sends one network search through the SearchService (spelled as the release when there
// is one), and turns the files that come back into SearchResults: quality, fit to the release, and what the user
// already has. The Search view filters and sorts them (SearchFilter); downloads go to the transfers (setDownloads())
// and land in the Inbox, tagged from the release.
//
// Results are analysed on a worker, one batch at a time; the analyses come back through a mutex-guarded mailbox.
// Searches live until closed (at most kMaxOpen); their results stay in the search cache for a day (SRCH-12).
//
// Threading: engine thread. Ownership: the SearchService outlives it; the destructor waits for the worker.
class ManualSearchService : public QObject
{
    Q_OBJECT

public:
    using ResolveFunction = std::function<QFuture<meta::Result<meta::Resolution>>(const meta::ResolveQuery &)>;
    // Fills in what only the engine knows about results, on the engine thread: Queued (in the transfers), downloads
    // of this session, the owned file's name, the peer's counters.
    using Annotate = std::function<void(QList<SearchResult> &results)>;
    // Queues a download (the TransferService in the app); false when it couldn't.
    using Enqueue = std::function<bool(const DownloadSpec &spec)>;

    static constexpr int kMaxOpen = 20;
    // How long a search waits for its release before it is sent as typed (the release still ranks the results).
    static constexpr std::chrono::milliseconds kLookupWait{6000};

    explicit ManualSearchService(SearchService &search, QObject *parent = nullptr);
    ~ManualSearchService() override;
    ManualSearchService(const ManualSearchService &) = delete;
    ManualSearchService &operator=(const ManualSearchService &) = delete;
    ManualSearchService(ManualSearchService &&) = delete;
    ManualSearchService &operator=(ManualSearchService &&) = delete;

    // Collaborators; each may be left unset (tests). Not owned.
    void setResolver(ResolveFunction resolve) { m_resolve = std::move(resolve); }
    void setExcludedPhrases(std::function<QStringList()> phrases) { m_excludedPhrases = std::move(phrases); }
    void setDownloads(Enqueue enqueue) { m_enqueue = std::move(enqueue); }
    // How long a search waits for its release before it goes as typed (tests shorten it).
    void setLookupWait(std::chrono::milliseconds wait) { m_lookupWait = wait; }
    void setAnnotate(Annotate annotate) { m_annotate = std::move(annotate); }
    // A new collection, Inbox and history snapshot: every open search is looked up again.
    void setLibraryIndex(std::shared_ptr<const match::MatchIndex> index);
    // Something about the transfers or the peers changed: results are annotated again.
    void refreshAnnotations();

    // Opens a search for `text`, or returns the open search with the same text. 0 for text with nothing to search.
    qint64 open(const QString &text);
    void close(qint64 id);
    // Ranks the results against another release candidate, or none (-1: "Any (name only)"). No network search.
    void chooseRelease(qint64 id, int candidate);
    // Searches the network again with the current choice (one search from the budget; the governor may refuse a
    // repeat it saw lately, and says so).
    void refresh(qint64 id);
    // "Search wider": the next query of the ladder (fewer words), for one search from the budget.
    void widen(qint64 id);

    // Downloads these results into the Inbox (DL-3): a result that fits the chosen release is tagged from it; any other
    // is looked up on its own first. Returns how many were accepted.
    int download(qint64 id, const QList<int> &indexes);

    QList<ManualSearch> searches() const;
    std::optional<ManualSearch> search(qint64 id) const;
    QList<SearchResult> results(qint64 id) const;
    std::optional<SearchResult> result(qint64 id, int index) const;

    // Blocks until the worker is idle and its results are recorded. For tests.
    void waitForIdle();

Q_SIGNALS:
    void searchChanged(const selecta::core::ManualSearch &search);
    void searchClosed(qint64 id);
    // New results, appended in index order.
    void resultsAdded(qint64 id, const QList<selecta::core::SearchResult> &results);
    // Every result again (another release, a new snapshot, a new network search).
    void resultsReset(qint64 id, const QList<selecta::core::SearchResult> &results);
    // Some results changed (a download started, annotations moved).
    void resultsChanged(qint64 id, const QList<selecta::core::SearchResult> &results);

private Q_SLOTS:
    void drain();

private:
    struct Session
    {
        ManualSearch search;
        QString key; // the text, normalised: one open search per text
        QList<SearchResult> results;
        QList<SearchHit> hits;      // every file of the current network search, in arrival order
        qsizetype analysedUpTo = 0; // hits handed to the worker under the current generation
        quint64 networkSearch = 0;  // the SearchService id in flight, 0 when none
        quint64 sendId = 0;         // bumps with each network search: callbacks of older ones are ignored
        quint64 generation = 0;     // bumps when results are reset: older analyses are dropped
        int widerStep = 0;          // 0: the default query, then widerLadder()'s steps
        bool analysing = false;     // a batch of the current generation is with the worker
        bool resetPending = false;  // the next batch replaces every result
        bool replacing = false;     // a new network search is out: its first answers replace the old ones
        QFuture<meta::Result<meta::Resolution>> lookup;
        std::unique_ptr<QTimer> lookupWait;
    };
    // What the worker sends back.
    struct Analysis
    {
        qint64 id = 0;
        quint64 generation = 0;
        bool reset = false; // replaces every result (else appends)
        QList<SearchResult> results;
    };

    Session *find(qint64 id);
    const Session *find(qint64 id) const;
    void lookedUp(qint64 id, const meta::Result<meta::Resolution> &result);
    void choose(Session &session, int candidate, bool automatically);
    void send(Session &session, bool refresh);
    void startReplacing(Session &session);
    void progress(qint64 id, quint64 sendId, const QList<SearchHit> &added, int peers);
    void finished(qint64 id, quint64 sendId, const SearchOutcome &outcome);
    // Hands hits waiting for analysis to the worker (one batch per search at a time).
    void analyseNext(Session &session);
    void reanalyse(Session &session); // every result again: new release or snapshot
    void post(Analysis analysis);
    void publish(const Session &session);
    std::optional<match::ReleaseReference> reference(const Session &session) const;
    QStringList excludedPhrases() const;
    void startDownload(const SearchResult &result, const std::optional<meta::Release> &release);

    SearchService &m_search;
    ResolveFunction m_resolve;
    std::function<QStringList()> m_excludedPhrases;
    Enqueue m_enqueue;
    Annotate m_annotate;
    std::chrono::milliseconds m_lookupWait = kLookupWait;
    std::shared_ptr<const match::MatchIndex> m_index;
    std::map<qint64, Session> m_sessions; // by id: the oldest first
    qint64 m_nextId = 1;
    QThreadPool m_pool;
    std::mutex m_mutex;
    QList<Analysis> m_analyses; // guarded by m_mutex
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::ManualSearch)
