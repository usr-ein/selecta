// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/EngineHost.h"

#include "governor/Limits.h"

#include <QLoggingCategory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcEngineHost, "selecta.core.enginehost")

} // namespace

EngineHost::EngineHost(EngineOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_paused(m_options.startPaused)
    , m_searchesAvailable(governor::limits::kSearchBurst)
{
    qRegisterMetaType<ActivityEntry>();
    qRegisterMetaType<InboxEntry>();
    qRegisterMetaType<QList<InboxEntry>>();
    qRegisterMetaType<SpotifyStatus>();
    qRegisterMetaType<GetLine>();
    qRegisterMetaType<QList<GetLine>>();
    qRegisterMetaType<NetworkStatus>();
    qRegisterMetaType<Preview>();
    qRegisterMetaType<CollectionStatus>();
    qRegisterMetaType<std::shared_ptr<audio::ByteSource>>();
    qRegisterMetaType<ManualSearch>();
    qRegisterMetaType<SearchResult>();
    qRegisterMetaType<QList<SearchResult>>();
    m_thread.setObjectName(u"selecta-engine"_s);
}

EngineHost::~EngineHost()
{
    stop();
}

void EngineHost::start()
{
    if (m_thread.isRunning()) {
        return;
    }
    auto *engine = new Engine(m_options); // deleted on its own thread when the thread finishes
    engine->moveToThread(&m_thread);
    m_engine = engine;
    connect(&m_thread, &QThread::finished, engine, &QObject::deleteLater);

    // Engine → UI: queued across the threads, so these run on the UI thread.
    connect(engine, &Engine::startFinished, this, [this](bool ok, const QString &error) {
        if (!ok) {
            qCCritical(lcEngineHost).noquote() << "engine didn't start:" << error;
        }
        Q_EMIT started(ok, error);
    });
    connect(engine, &Engine::pausedChanged, this, [this](bool paused) {
        m_paused = paused;
        Q_EMIT pausedChanged(paused);
    });
    connect(engine, &Engine::searchBudgetChanged, this, [this](int available) {
        m_searchesAvailable = available;
        Q_EMIT searchBudgetChanged(available);
    });
    connect(engine, &Engine::activityRecorded, this, &EngineHost::activityRecorded);
    connect(engine, &Engine::inboxChanged, this, [this](const QList<InboxEntry> &entries) {
        m_inboxEntries = entries;
        Q_EMIT inboxChanged(entries);
    });
    connect(engine, &Engine::inboxOperationFinished, this, &EngineHost::inboxOperationFinished);
    connect(engine, &Engine::tracksAdded, this, &EngineHost::tracksAdded);
    connect(engine, &Engine::searchQueueChanged, this, &EngineHost::searchQueueChanged);
    connect(engine, &Engine::previewStarted, this, &EngineHost::previewStarted);
    connect(engine, &Engine::previewRefused, this, &EngineHost::previewRefused);
    connect(engine, &Engine::previewChanged, this, &EngineHost::previewChanged);
    connect(engine, &Engine::collectionStatusChanged, this, [this](const CollectionStatus &status) {
        m_collectionStatus = status;
        Q_EMIT collectionStatusChanged(status);
    });
    connect(engine, &Engine::searchOpened, this, &EngineHost::searchOpened);
    connect(engine, &Engine::manualSearchChanged, this, [this](const ManualSearch &search) {
        const auto known = std::ranges::find(m_searches, search.id, &ManualSearch::id);
        if (known == m_searches.end()) {
            m_searches.append(search);
        } else {
            *known = search;
        }
        Q_EMIT manualSearchChanged(search);
    });
    connect(engine, &Engine::manualSearchClosed, this, [this](qint64 searchId) {
        m_searches.removeIf([searchId](const ManualSearch &search) { return search.id == searchId; });
        m_searchResults.remove(searchId);
        Q_EMIT manualSearchClosed(searchId);
    });
    connect(engine, &Engine::searchResultsAdded, this, [this](qint64 searchId, const QList<SearchResult> &results) {
        m_searchResults[searchId].append(results);
        Q_EMIT searchResultsAdded(searchId, results);
    });
    connect(engine, &Engine::searchResultsReset, this, [this](qint64 searchId, const QList<SearchResult> &results) {
        m_searchResults.insert(searchId, results);
        Q_EMIT searchResultsReset(searchId, results);
    });
    connect(engine, &Engine::searchResultsChanged, this, [this](qint64 searchId, const QList<SearchResult> &results) {
        QList<SearchResult> &known = m_searchResults[searchId];
        for (const SearchResult &result : results) {
            if (result.index >= 0 && result.index < known.size()) {
                known[result.index] = result;
            }
        }
        Q_EMIT searchResultsChanged(searchId, results);
    });
    connect(engine, &Engine::networkStatusChanged, this, [this](const NetworkStatus &status) {
        m_networkStatus = status;
        Q_EMIT networkStatusChanged(status);
    });
    connect(engine, &Engine::spotifyStatusChanged, this, [this](const SpotifyStatus &status) {
        m_spotifyStatus = status;
        Q_EMIT spotifyStatusChanged(status);
    });
    connect(engine, &Engine::getLinesReset, this, [this](const QList<GetLine> &lines) {
        m_getLines = lines;
        Q_EMIT getLinesReset(lines);
    });
    connect(engine, &Engine::getLineChanged, this, [this](const GetLine &line) {
        for (GetLine &known : m_getLines) {
            if (known.id == line.id) {
                known = line;
                break;
            }
        }
        Q_EMIT getLineChanged(line);
    });

    m_thread.start();
    QMetaObject::invokeMethod(engine, &Engine::start, Qt::QueuedConnection);
}

void EngineHost::stop()
{
    if (!m_thread.isRunning()) {
        return;
    }
    if (m_engine) {
        // Waits for the engine to close its services and database on its own thread.
        QMetaObject::invokeMethod(m_engine.data(), &Engine::stop, Qt::BlockingQueuedConnection);
    }
    m_thread.quit();
    m_thread.wait();
}

void EngineHost::setPaused(bool paused)
{
    post([paused](Engine &engine) { engine.setPaused(paused); });
}

void EngineHost::setLibraryConfig(const LibraryConfig &config)
{
    m_options.library = config; // used if the engine (re)starts
    post([config](Engine &engine) {
        if (engine.isStarted()) {
            engine.setLibraryConfig(config);
        }
    });
}

void EngineHost::refreshInbox()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.inbox().reconcile();
        }
    });
}

void EngineHost::fileInboxItems(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.inbox().file(ids);
        }
    });
}

void EngineHost::trashInboxItems(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.inbox().trash(ids);
        }
    });
}

void EngineHost::setSpotifyCredentials(const QString &clientId, const QByteArray &secret, bool keepSavedSecret)
{
    post([clientId, secret, keepSavedSecret](Engine &engine) {
        if (engine.isStarted()) {
            engine.meta().setSpotifyCredentials(clientId, secret, keepSavedSecret);
        }
    });
}

void EngineHost::signInToSpotify()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.meta().signInToSpotify();
        }
    });
}

void EngineHost::signOutOfSpotify()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.meta().signOutOfSpotify();
        }
    });
}

void EngineHost::addTracks(const QString &text)
{
    post([text](Engine &engine) { engine.addTracks(text); });
}

void EngineHost::removeTracks(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().remove(ids);
        }
    });
}

void EngineHost::clearTracks()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().clear();
        }
    });
}

void EngineHost::tickTracks(const QList<qint64> &ids, bool ticked)
{
    post([ids, ticked](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().setTicked(ids, ticked);
        }
    });
}

void EngineHost::retryTracks(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().retry(ids);
        }
    });
}

void EngineHost::scanCollection()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.collection().scanNow();
        }
    });
}

void EngineHost::previewTrack(qint64 lineId)
{
    post([lineId](Engine &engine) { engine.previewLine(lineId); });
}

void EngineHost::stopPreview()
{
    post([](Engine &engine) { engine.stopPreview(); });
}

quint64 EngineHost::openSearch(const QString &text)
{
    const quint64 request = m_nextSearchRequest++;
    post([request, text](Engine &engine) { engine.openSearch(request, text); });
    return request;
}

void EngineHost::closeSearch(qint64 searchId)
{
    post([searchId](Engine &engine) {
        if (engine.isStarted()) {
            engine.manualSearch().close(searchId);
        }
    });
}

void EngineHost::chooseSearchRelease(qint64 searchId, int candidate)
{
    post([searchId, candidate](Engine &engine) {
        if (engine.isStarted()) {
            engine.manualSearch().chooseRelease(searchId, candidate);
        }
    });
}

void EngineHost::refreshSearch(qint64 searchId)
{
    post([searchId](Engine &engine) {
        if (engine.isStarted()) {
            engine.manualSearch().refresh(searchId);
        }
    });
}

void EngineHost::widenSearch(qint64 searchId)
{
    post([searchId](Engine &engine) {
        if (engine.isStarted()) {
            engine.manualSearch().widen(searchId);
        }
    });
}

void EngineHost::downloadResults(qint64 searchId, const QList<int> &indexes)
{
    post([searchId, indexes](Engine &engine) {
        if (engine.isStarted()) {
            engine.manualSearch().download(searchId, indexes);
        }
    });
}

void EngineHost::previewResult(qint64 searchId, int index)
{
    post([searchId, index](Engine &engine) { engine.previewResult(searchId, index); });
}

std::optional<ManualSearch> EngineHost::search(qint64 searchId) const
{
    const auto found = std::ranges::find(m_searches, searchId, &ManualSearch::id);
    return found == m_searches.end() ? std::nullopt : std::optional<ManualSearch>(*found);
}

void EngineHost::downloadTracks(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().download(ids);
        }
    });
}

void EngineHost::cancelTrackDownloads(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().cancelDownloads(ids);
        }
    });
}

void EngineHost::findTracks(const QList<qint64> &ids)
{
    post([ids](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().find(ids);
        }
    });
}

void EngineHost::stopFindingTracks()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.getTracks().stopFinding();
        }
    });
}

void EngineHost::setNetworkConfig(const NetworkConfig &config)
{
    m_options.network = config;
    post([config](Engine &engine) {
        if (engine.isStarted()) {
            engine.network().setConfig(config);
        }
    });
}

void EngineHost::setNetworkPassword(const QString &password)
{
    post([password](Engine &engine) {
        if (engine.isStarted()) {
            engine.network().setPassword(password);
        }
    });
}

void EngineHost::connectNetwork()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.network().connectNow();
        }
    });
}

void EngineHost::disconnectNetwork()
{
    post([](Engine &engine) {
        if (engine.isStarted()) {
            engine.network().disconnectNow();
        }
    });
}

} // namespace selecta::core
