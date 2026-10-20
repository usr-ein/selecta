// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchService.h"

#include "core/SearchCache.h"
#include "governor/Limits.h"
#include "governor/QueryHygiene.h"

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcSearch, "selecta.core.search")

namespace limits = governor::limits;

QString extensionOf(const slsk::FileEntry &file)
{
    QString extension = file.extension.trimmed().toLower();
    if (extension.isEmpty()) {
        const QString name = file.name.display();
        const qsizetype dot = name.lastIndexOf(u'.');
        const qsizetype separator = std::max(name.lastIndexOf(u'\\'), name.lastIndexOf(u'/'));
        if (dot > separator && dot + 1 < name.size()) {
            extension = name.mid(dot + 1).toLower();
        }
    }
    if (extension.startsWith(u'.')) {
        extension.remove(0, 1);
    }
    return extension.left(8);
}

} // namespace

SearchService::SearchService(governor::Governor &governor, SearchNetwork &network, const base::Clock &clock,
                             SearchTiming timing, QObject *parent)
    : QObject(parent)
    , m_governor(governor)
    , m_network(network)
    , m_clock(clock)
    , m_timing{.idle = std::min<std::chrono::milliseconds>(timing.idle, limits::kResultsIdle),
               .total = std::min<std::chrono::milliseconds>(timing.total, limits::kResultsTotal)}
    , m_retry(new QTimer(this))
{
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &SearchService::pump);
}

SearchService::~SearchService()
{
    for (const auto &[token, running] : m_running) {
        m_network.stopSearch(token);
    }
}

quint64 SearchService::enqueue(const Request &request, Done done, Started started, Progress progress)
{
    const quint64 id = m_nextId++;
    const bool manual = request.source == governor::SearchSource::Manual;
    Pending pending{.id = id,
                    .request = request,
                    .done = std::move(done),
                    .started = std::move(started),
                    .progress = std::move(progress)};
    (manual ? m_manual : m_batch).push_back(std::move(pending));
    // Sent on the next turn, so a caller queueing many searches never sees `done` run inside enqueue().
    QMetaObject::invokeMethod(this, &SearchService::pump, Qt::QueuedConnection);
    publish();
    return id;
}

void SearchService::cancel(quint64 id)
{
    const auto drop = [id](std::deque<Pending> &queue) {
        return std::erase_if(queue, [id](const Pending &pending) { return pending.id == id; }) > 0;
    };
    if (drop(m_manual) || drop(m_batch)) {
        publish();
        return;
    }
    for (auto it = m_running.begin(); it != m_running.end(); ++it) {
        if (it->second.id == id) {
            m_network.stopSearch(it->first);
            m_running.erase(it);
            publish();
            return;
        }
    }
}

void SearchService::networkChanged()
{
    if (!m_network.isOnline()) {
        // Collections in flight end with what they have; their tokens died with the session.
        QList<quint32> tokens;
        for (const auto &[token, running] : m_running) {
            tokens.append(token);
        }
        for (const quint32 token : tokens) {
            finish(token);
        }
    }
    pump();
}

void SearchService::pump()
{
    m_retry->stop();
    m_nextInMs = 0;
    // Searches typed by hand first; Get tracks waits behind them.
    while (!m_manual.empty() && sendFront(m_manual)) {
    }
    if (m_manual.empty()) {
        while (!m_batch.empty() && sendFront(m_batch)) {
        }
    }
    publish();
}

std::optional<SearchOutcome> SearchService::cached(const QString &key)
{
    if (const auto found = m_cache.constFind(key); found != m_cache.cend()) {
        if (m_clock.now() - found->at < kCacheLifetime) {
            SearchOutcome outcome = found->outcome;
            outcome.fromCache = true;
            return outcome;
        }
        m_cache.erase(found);
    }
    if (m_store) {
        if (std::optional<SearchCacheStore::Entry> entry = m_store->find(key)) {
            return std::move(entry->outcome);
        }
    }
    return std::nullopt;
}

bool SearchService::sendFront(std::deque<Pending> &queue)
{
    Pending &front = queue.front();
    const governor::CleanQuery clean = governor::cleanQuery(front.request.query, m_governor.excludedPhrases());
    if (clean.refusal == governor::Refusal::None && !front.request.refresh) {
        // A repeat within the cache lifetime costs nothing.
        if (std::optional<SearchOutcome> outcome = cached(governor::queryKey(clean.query))) {
            deliver(std::move(front.done), std::move(*outcome));
            queue.pop_front();
            return true;
        }
    }
    if (m_governor.isPaused() || !m_network.isOnline()) {
        return false; // networkChanged() resumes
    }
    governor::Decision decision =
        m_governor.request(governor::Operation::Search, {.query = front.request.query, .source = front.request.source});
    if (decision.permit) {
        const QString query = decision.cleanedQuery;
        const quint32 token = front.request.source == governor::SearchSource::Wishlist
                                  ? m_network.searchWishlist(query, std::move(*decision.permit))
                                  : m_network.search(query, std::move(*decision.permit));
        if (token == 0) {
            return false; // went offline meanwhile: networkChanged() resumes
        }
        if (front.started) {
            front.started();
        }
        Running running{.id = front.id,
                        .query = query,
                        .key = governor::queryKey(query),
                        .done = std::move(front.done),
                        .progress = std::move(front.progress),
                        .hits = {},
                        .peers = {},
                        .idle = std::make_unique<QTimer>(),
                        .total = std::make_unique<QTimer>()};
        running.idle->setSingleShot(true);
        running.idle->setInterval(m_timing.idle);
        running.total->setSingleShot(true);
        running.total->setInterval(m_timing.total);
        connect(running.idle.get(), &QTimer::timeout, this, [this, token] { finish(token); });
        connect(running.total.get(), &QTimer::timeout, this, [this, token] { finish(token); });
        running.idle->start();
        running.total->start();
        m_running.emplace(token, std::move(running));
        queue.pop_front();
        return true;
    }
    switch (decision.refusal) {
    case governor::Refusal::RateLimited:
    case governor::Refusal::NetworkPaused: {
        const auto wait = std::max<base::Clock::Duration>(decision.retryAfter, 100ms);
        m_nextInMs = std::chrono::duration_cast<std::chrono::milliseconds>(wait).count();
        m_retry->start(std::chrono::duration_cast<std::chrono::milliseconds>(wait));
        return false;
    }
    default:
        // Too short, excluded by the server, or a repeat whose results were evicted: nothing to send.
        deliver(std::move(front.done), SearchOutcome{.query = clean.query,
                                                     .hits = {},
                                                     .peers = 0,
                                                     .fromCache = false,
                                                     .searchedAtMs = 0,
                                                     .error = decision.explanation});
        queue.pop_front();
        return true;
    }
}

void SearchService::received(quint32 token, const QString &username, const slsk::peer::FileSearchResponse &response)
{
    const auto found = m_running.find(token);
    if (found == m_running.end()) {
        return;
    }
    Running &running = found->second;
    running.peers.insert(username);
    const qsizetype before = running.hits.size();
    const auto add = [&](const QList<slsk::FileEntry> &files) {
        for (const slsk::FileEntry &file : files) {
            if (running.hits.size() >= limits::kResultsFiles) {
                return;
            }
            running.hits.append(SearchHit{.peer = username,
                                          .path = file.name.raw,
                                          .size = file.size,
                                          .extension = extensionOf(file),
                                          .attributes = file.attributes,
                                          .freeSlot = response.slotFree,
                                          .queueLength = response.queueLength,
                                          .averageSpeed = response.averageSpeed});
        }
    };
    add(response.results); // private results are shared with some users only: not offered for download
    if (running.progress && running.hits.size() > before) {
        // On the next turn, like `done`: queued in order, so every progress call comes before `done`.
        QMetaObject::invokeMethod(
            this,
            [progress = running.progress, added = running.hits.mid(before),
             peers = static_cast<int>(running.peers.size())] { progress(added, peers); },
            Qt::QueuedConnection);
    }
    if (running.peers.size() >= limits::kResultsPeers || running.hits.size() >= limits::kResultsFiles) {
        finish(token);
        return;
    }
    running.idle->start(); // results still coming: wait for them to stop
}

void SearchService::finish(quint32 token)
{
    const auto found = m_running.find(token);
    if (found == m_running.end()) {
        return;
    }
    Running running = std::move(found->second);
    m_running.erase(found);
    m_network.stopSearch(token);
    SearchOutcome outcome{.query = running.query,
                          .hits = std::move(running.hits),
                          .peers = static_cast<int>(running.peers.size()),
                          .fromCache = false,
                          .searchedAtMs = 0,
                          .error = {}};
    m_cache.insert(running.key, Cached{.outcome = outcome, .at = m_clock.now()});
    if (m_store && !m_store->store(running.key, outcome)) {
        qCWarning(lcSearch) << "couldn't keep the results of" << running.query << "for later";
    }
    qCDebug(lcSearch) << "search" << running.query << "collected" << outcome.hits.size() << "files from"
                      << outcome.peers << "peers";
    deliver(std::move(running.done), std::move(outcome));
    publish();
}

void SearchService::deliver(Done done, SearchOutcome outcome)
{
    if (!done) {
        return;
    }
    // On the next turn, so callbacks never run inside enqueue(), cancel() or the network's signal.
    QMetaObject::invokeMethod(
        this, [done = std::move(done), outcome = std::move(outcome)] { done(outcome); }, Qt::QueuedConnection);
}

void SearchService::publish()
{
    Q_EMIT queueChanged(waiting(), collecting(), m_nextInMs);
}

} // namespace selecta::core
