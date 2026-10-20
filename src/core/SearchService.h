// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "slsk/PeerMessages.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <memory>

class QTimer;

namespace selecta::core {

// What SearchService needs from the network: NetworkService in the app, a fake in tests.
class SearchNetwork
{
public:
    SearchNetwork() = default;
    SearchNetwork(const SearchNetwork &) = delete;
    SearchNetwork &operator=(const SearchNetwork &) = delete;
    SearchNetwork(SearchNetwork &&) = delete;
    SearchNetwork &operator=(SearchNetwork &&) = delete;
    virtual ~SearchNetwork() = default;

    virtual bool isOnline() const = 0;
    // Sends the search and opens its token for results; 0 when it couldn't (the permit is dropped).
    virtual quint32 search(const QString &query, governor::Permit permit) = 0;
    virtual void stopSearch(quint32 token) = 0;
};

// How long a search's results are collected. Defaults are the governor's limits; tests shorten them (never lengthen:
// SearchService caps both at the limits).
struct SearchTiming
{
    std::chrono::milliseconds idle = governor::limits::kResultsIdle;
    std::chrono::milliseconds total = governor::limits::kResultsTotal;
};

// One file in a search result.
struct SearchHit
{
    QString peer;
    QByteArray path; // the full virtual path, as the peer sent it
    quint64 size = 0;
    QString extension; // lower case; from the name when the peer didn't send one
    slsk::FileAttributes attributes;
    bool freeSlot = false;
    quint32 queueLength = 0;
    quint32 averageSpeed = 0; // bytes per second
};

struct SearchOutcome
{
    QString query; // as sent, after the governor's cleaning
    QList<SearchHit> hits;
    int peers = 0;
    bool fromCache = false; // answered from a recent identical search, spending no budget (BATCH-4)
    QString error;          // why no search could be sent; hits are empty then
};

// Searches the Soulseek network through the governor (ADR-006, SAFE-1): a search is sent only when the governor's
// budget allows, then its results are collected until they stop coming (kResultsIdle) or for at most kResultsTotal,
// up to kResultsPeers peers and kResultsFiles files. A repeat of a recent search is answered from the cache without
// spending budget. Searches typed by hand go ahead of Get tracks' queue, which also leaves part of the budget for
// them (the governor's reserve).
//
// Threading: engine thread. The network's results and state changes are fed in through received() and
// networkChanged().
class SearchService : public QObject
{
    Q_OBJECT

public:
    using Done = std::function<void(const SearchOutcome &outcome)>;
    using Started = std::function<void()>;

    static constexpr std::chrono::minutes kCacheLifetime{30};

    SearchService(governor::Governor &governor, SearchNetwork &network, const base::Clock &clock,
                  SearchTiming timing = {}, QObject *parent = nullptr);
    ~SearchService() override;
    SearchService(const SearchService &) = delete;
    SearchService &operator=(const SearchService &) = delete;
    SearchService(SearchService &&) = delete;
    SearchService &operator=(SearchService &&) = delete;

    // Queues a search; `done` runs once, on this thread, with the results or the reason there are none. Returns an id
    // for cancel().
    quint64 enqueue(const QString &query, governor::SearchSource source, Done done, Started started = {});
    // Drops a queued search, or stops collecting a running one (its `done` doesn't run).
    void cancel(quint64 id);
    int waiting() const { return static_cast<int>(m_manual.size() + m_batch.size()); }
    int collecting() const { return static_cast<int>(m_running.size()); }

    // --- fed by the network ---
    void received(quint32 token, const QString &username, const slsk::peer::FileSearchResponse &response);
    // Online or offline; paused or resumed. Retries what was waiting.
    void networkChanged();

Q_SIGNALS:
    // The queue moved: searches waiting, collecting, and how long until the governor allows the next (0: now).
    void queueChanged(int waiting, int collecting, qint64 nextInMs);

private:
    struct Pending
    {
        quint64 id = 0;
        QString query;
        governor::SearchSource source = governor::SearchSource::Manual;
        Done done;
        Started started;
    };
    struct Running
    {
        quint64 id = 0;
        QString query;
        QString key;
        Done done;
        QList<SearchHit> hits;
        QSet<QString> peers;
        std::unique_ptr<QTimer> idle;
        std::unique_ptr<QTimer> total;
    };
    struct Cached
    {
        SearchOutcome outcome;
        base::Clock::TimePoint at;
    };

    void pump();
    // Tries to send the front of `queue`; returns false when the queue has to wait.
    bool sendFront(std::deque<Pending> &queue);
    void finish(quint32 token);
    void deliver(Done done, SearchOutcome outcome);
    void publish();

    governor::Governor &m_governor;
    SearchNetwork &m_network;
    const base::Clock &m_clock;
    SearchTiming m_timing;
    std::deque<Pending> m_manual;
    std::deque<Pending> m_batch;
    std::map<quint32, Running> m_running; // token → collection
    QHash<QString, Cached> m_cache;       // query key → recent results
    QTimer *m_retry = nullptr;
    quint64 m_nextId = 1;
    qint64 m_nextInMs = 0;
};

} // namespace selecta::core
