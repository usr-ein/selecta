// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "core/CountryDatabase.h"
#include "governor/Governor.h"
#include "governor/Limits.h"
#include "slsk/PeerMessages.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

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
    // The same for a wishlist search (the server's WishlistSearch message); a plain search where there is none.
    virtual quint32 searchWishlist(const QString &query, governor::Permit permit)
    {
        return search(query, std::move(permit));
    }
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
    // The peer's country (ISO 3166-1 alpha-2, "FR") from the address its results came from, for its flag (P-002);
    // empty when unknown. Not kept in the search cache across launches.
    QString country;
};

struct SearchOutcome
{
    QString query; // as sent, after the governor's cleaning
    QList<SearchHit> hits;
    int peers = 0;
    bool fromCache = false;  // answered from a recent identical search, spending no budget (BATCH-4, SRCH-12)
    qint64 searchedAtMs = 0; // when the network search ran (Unix time, ms); 0 when unknown
    QString error;           // why no search could be sent; hits are empty then
};

class SearchCacheStore;

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
    // Results as they arrive: the files of one peer's answer, and the peers that answered so far. `done` still gets
    // every file, in the same order.
    using Progress = std::function<void(const QList<SearchHit> &added, int peers)>;

    struct Request
    {
        QString query;
        governor::SearchSource source = governor::SearchSource::Manual;
        bool refresh = false; // ask the network even when a recent answer is kept (the governor still decides)
    };

    static constexpr std::chrono::minutes kCacheLifetime{30};

    SearchService(governor::Governor &governor, SearchNetwork &network, const base::Clock &clock,
                  SearchTiming timing = {}, QObject *parent = nullptr);
    ~SearchService() override;
    SearchService(const SearchService &) = delete;
    SearchService &operator=(const SearchService &) = delete;
    SearchService(SearchService &&) = delete;
    SearchService &operator=(SearchService &&) = delete;

    // Queues a search; `done` runs once, on this thread, with the results or the reason there are none; `progress`
    // (optional) runs as results arrive. Returns an id for cancel().
    quint64 enqueue(const Request &request, Done done, Started started = {}, Progress progress = {});
    quint64 enqueue(const QString &query, governor::SearchSource source, Done done, Started started = {})
    {
        return enqueue(Request{.query = query, .source = source, .refresh = false}, std::move(done),
                       std::move(started));
    }
    // Where finished searches are kept across launches (SRCH-12); null keeps them in memory only, for
    // kCacheLifetime. Not owned; it must outlive this service or be unset first.
    void setStore(SearchCacheStore *store) { m_store = store; }
    // Where peers' countries come from (P-002); null (the default): no countries. Any thread may hold it too.
    void setCountries(std::shared_ptr<const CountryDatabase> countries) { m_countries = std::move(countries); }
    // Drops a queued search, or stops collecting a running one (its `done` doesn't run).
    void cancel(quint64 id);
    int waiting() const { return static_cast<int>(m_manual.size() + m_batch.size()); }
    int collecting() const { return static_cast<int>(m_running.size()); }

    // --- fed by the network ---
    // `address`: where the results came from (null when unknown), looked up for the peer's country.
    void received(quint32 token, const QString &username, const slsk::peer::FileSearchResponse &response,
                  const QHostAddress &address = {});
    // Online or offline; paused or resumed. Retries what was waiting.
    void networkChanged();

Q_SIGNALS:
    // The queue moved: searches waiting, collecting, and how long until the governor allows the next (0: now).
    void queueChanged(int waiting, int collecting, qint64 nextInMs);

private:
    struct Pending
    {
        quint64 id = 0;
        Request request;
        Done done;
        Started started;
        Progress progress;
    };
    struct Running
    {
        quint64 id = 0;
        QString query;
        QString key;
        Done done;
        Progress progress;
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
    // A recent answer to the query with this key, from memory or the store.
    std::optional<SearchOutcome> cached(const QString &key);
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
    SearchCacheStore *m_store = nullptr;
    std::shared_ptr<const CountryDatabase> m_countries;
    QTimer *m_retry = nullptr;
    quint64 m_nextId = 1;
    qint64 m_nextInMs = 0;
};

} // namespace selecta::core
