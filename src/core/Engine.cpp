// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "base/Version.h"
#include "core/GovernorStore.h"
#include "core/ResultAnnotator.h"
#include "governor/Limits.h"
#include "library/HistoryStore.h"
#include "meta/SpotifyLinks.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcEngine, "selecta.core.engine")

constexpr auto kActivityPruneInterval = 6h;

// SearchService's view of the network service.
class NetworkSearch final : public SearchNetwork
{
public:
    explicit NetworkSearch(NetworkService &network)
        : m_network(network)
    {}

    bool isOnline() const override { return m_network.isOnline(); }
    quint32 search(const QString &query, governor::Permit permit) override
    {
        return m_network.search(query, std::move(permit));
    }
    void stopSearch(quint32 token) override { m_network.stopSearch(token); }

private:
    NetworkService &m_network;
};

} // namespace

Engine::Engine(EngineOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    if (!m_options.wallClock) {
        m_options.wallClock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    }
    if (m_options.clock) {
        m_clock = m_options.clock;
    } else {
        m_ownClock = std::make_unique<base::SteadyClock>();
        m_clock = m_ownClock.get();
    }
}

Engine::~Engine()
{
    stop();
}

bool Engine::fail(const QString &error)
{
    m_error = error;
    qCCritical(lcEngine).noquote() << "engine can't start:" << error;
    stop();
    Q_EMIT startFinished(false, error);
    return false;
}

bool Engine::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_started) {
        return true;
    }
    m_error.clear();

    m_db = std::make_unique<storage::Database>();
    if (!m_db->open(m_options.databasePath)) {
        return fail(tr("Can't open the library: %1").arg(m_db->lastError()));
    }
    // The UI thread migrates first at launch; this is a no-op then, and makes the engine usable on its own.
    const auto migration = storage::migrate(*m_db, storage::selectaMigrations());
    if (!migration.ok()) {
        return fail(tr("Can't update the library: %1").arg(migration.error));
    }

    m_governor = std::make_unique<governor::Governor>(*m_clock, m_options.governor);
    m_activity = std::make_unique<ActivityLog>(*m_db, m_options.wallClock);
    connect(m_governor.get(), &governor::Governor::decided, m_activity.get(), &ActivityLog::recordDecision);
    connect(m_activity.get(), &ActivityLog::recorded, this, &Engine::activityRecorded);
    connect(m_governor.get(), &governor::Governor::pausedChanged, this, &Engine::pausedChanged);
    connect(m_governor.get(), &governor::Governor::searchBudgetChanged, this, &Engine::reportSearchBudget);

    m_governorStore = std::make_unique<GovernorStore>(*m_db, *m_governor, m_options.wallClock);
    if (!m_governorStore->restore()) {
        // Not fatal: the limits still apply from now on, only earlier totals are lost.
        qCWarning(lcEngine) << "couldn't restore the governor's saved state:" << m_db->lastError();
    }

    MetaServices::Options metaOptions;
    metaOptions.tokens = m_options.tokenStore ? m_options.tokenStore() : std::make_unique<meta::InMemoryTokenStore>();
    metaOptions.openUrl = m_options.openUrl;
    metaOptions.endpoints = m_options.metaEndpoints;
    m_meta = std::make_unique<MetaServices>(*m_governor, *m_activity, *m_clock, std::move(metaOptions));
    connect(m_meta.get(), &MetaServices::spotifyStatusChanged, this, &Engine::spotifyStatusChanged);

    m_network = std::make_unique<NetworkService>(*m_governor, *m_activity, m_meta->tokens(), m_options.wallClock);
    connect(m_network.get(), &NetworkService::statusChanged, this, &Engine::networkStatusChanged);

    m_searchNetwork = std::make_unique<NetworkSearch>(*m_network);
    m_search = std::make_unique<SearchService>(*m_governor, *m_searchNetwork, *m_clock);
    m_searchCache = std::make_unique<SearchCacheStore>(*m_db, m_options.wallClock);
    m_search->setStore(m_searchCache.get());
    connect(m_network.get(), &NetworkService::searchResponseReceived, m_search.get(), &SearchService::received);
    connect(m_network.get(), &NetworkService::statusChanged, m_search.get(), &SearchService::networkChanged);
    connect(m_governor.get(), &governor::Governor::pausedChanged, m_search.get(), &SearchService::networkChanged);
    connect(m_search.get(), &SearchService::queueChanged, this, &Engine::searchQueueChanged);

    m_getTracks = std::make_unique<GetTracksService>(*m_db, m_options.wallClock);
    m_getTracks->setSearch(m_search.get(), [this] { return m_governor->excludedPhrases(); });
    // Release lookups for a whole list go in the background lane: previews the user waits for overtake them.
    m_getTracks->setResolver([resolver = &m_meta->resolver()](const meta::ResolveQuery &query) {
        return resolver->resolve(query, meta::Priority::Background);
    });
    connect(m_getTracks.get(), &GetTracksService::linesReset, this, &Engine::getLinesReset);
    connect(m_getTracks.get(), &GetTracksService::lineChanged, this, &Engine::getLineChanged);

    m_inbox = std::make_unique<InboxService>(*m_db, m_options.library, m_options.trash);
    connect(m_inbox.get(), &InboxService::changed, this, &Engine::inboxChanged);
    connect(m_inbox.get(), &InboxService::finished, this, &Engine::inboxOperationFinished);

    // Downloads, then the arrival pipeline that turns each complete file into an Inbox item (D27, D29).
    TransferService::Options transferOptions;
    transferOptions.incomingDir = m_options.incomingDir;
    transferOptions.previewDir = m_options.previewDir;
    m_transfers = std::make_unique<TransferService>(*m_db, *m_governor, *m_network, *m_activity,
                                                    std::move(transferOptions), m_options.wallClock);
    ArrivalService::Options arrivalOptions;
    arrivalOptions.stagingDir = m_options.stagingDir;
    arrivalOptions.encodedBy = u"Selecta "_s + QString(base::kVersion);
    arrivalOptions.session = m_options.session;
    m_arrival = std::make_unique<ArrivalService>(*m_db, *m_inbox, *m_activity, m_options.trash,
                                                 std::move(arrivalOptions), m_options.wallClock);
    m_arrival->setMeta(m_meta.get());
    connect(m_transfers.get(), &TransferService::completed, m_arrival.get(), &ArrivalService::start);
    connect(m_transfers.get(), &TransferService::changed, m_arrival.get(),
            [arrival = m_arrival.get()](const Download &d) {
                if (d.state == Download::State::Cancelled) {
                    arrival->cancel(d.id);
                }
            });
    connect(m_arrival.get(), &ArrivalService::finished, this, [this](qint64 id, const ArrivalResult &result) {
        if (result.ok) {
            m_transfers->arrived(id, result.inboxItem);
        } else {
            m_transfers->arrivalFailed(id, result.message);
        }
    });
    m_getTracks->setTransfers(m_transfers.get());

    // Searches typed by hand: the release first, then one search, ranked against it (SRCH-1, SRCH-17).
    m_manualSearch = std::make_unique<ManualSearchService>(*m_search);
    m_manualSearch->setResolver([resolver = &m_meta->resolver()](const meta::ResolveQuery &query) {
        return resolver->resolve(query, meta::Priority::Interactive);
    });
    m_manualSearch->setExcludedPhrases([this] { return m_governor->excludedPhrases(); });
    m_manualSearch->setDownloads(
        [transfers = m_transfers.get()](const DownloadSpec &spec) { return transfers->enqueue(spec).has_value(); });
    m_manualSearch->setAnnotate([this](QList<SearchResult> &results) { annotate(results); });
    connect(m_manualSearch.get(), &ManualSearchService::searchChanged, this, &Engine::manualSearchChanged);
    connect(m_manualSearch.get(), &ManualSearchService::searchClosed, this, &Engine::manualSearchClosed);
    connect(m_manualSearch.get(), &ManualSearchService::resultsAdded, this, &Engine::searchResultsAdded);
    connect(m_manualSearch.get(), &ManualSearchService::resultsReset, this, &Engine::searchResultsReset);
    connect(m_manualSearch.get(), &ManualSearchService::resultsChanged, this, &Engine::searchResultsChanged);
    m_annotateTimer = new QTimer(this);
    m_annotateTimer->setSingleShot(true);
    m_annotateTimer->setInterval(300ms);
    connect(m_annotateTimer, &QTimer::timeout, m_manualSearch.get(), &ManualSearchService::refreshAnnotations);
    connect(m_transfers.get(), &TransferService::changed, m_annotateTimer, qOverload<>(&QTimer::start));

    // What the user owns: the collection index, with the Inbox and the history, for "you have it".
    CollectionService::Options collectionOptions;
    collectionOptions.watch = m_options.watchCollection;
    m_collection = std::make_unique<CollectionService>(*m_db, collectionOptions, m_options.wallClock);
    connect(m_collection.get(), &CollectionService::statusChanged, this, &Engine::collectionStatusChanged);
    connect(m_collection.get(), &CollectionService::indexChanged, this,
            [this](const std::shared_ptr<const match::MatchIndex> &index) {
                m_getTracks->setLibraryIndex(index);
                m_manualSearch->setLibraryIndex(index);
            });
    connect(m_inbox.get(), &InboxService::changed, m_collection.get(),
            [collection = m_collection.get()](const QList<InboxEntry> &entries) {
                QList<library::InboxItem> items;
                items.reserve(entries.size());
                for (const InboxEntry &entry : entries) {
                    items.append(entry.item);
                }
                collection->setInbox(items);
            });
    connect(m_arrival.get(), &ArrivalService::finished, m_collection.get(),
            [this](qint64 /*downloadId*/, const ArrivalResult &result) {
                if (result.ok) {
                    m_collection->setHistory(library::HistoryStore(*m_db).downloads());
                }
            });
    connect(m_transfers.get(), &TransferService::changed, m_getTracks.get(), &GetTracksService::downloadChanged);
    connect(m_transfers.get(), &TransferService::progress, m_getTracks.get(), &GetTracksService::downloadProgress);
    connect(m_transfers.get(), &TransferService::previewChanged, this, &Engine::previewChanged);
    m_youtube = std::make_unique<YouTubePreviews>(
        m_meta->ytDlp(), m_options.previewDir.isEmpty() ? QString() : m_options.previewDir + u"/YouTube"_s);
    connect(m_youtube.get(), &YouTubePreviews::started, this,
            [this](qint64 lineId, qint64 previewId, const std::shared_ptr<audio::ByteSource> &source,
                   const QString &identity, const QString &title, const QString &artist, const QString &label) {
                Q_EMIT previewStarted(previewKeyForLine(lineId), previewId, source, identity, title, artist, label);
            });
    connect(m_youtube.get(), &YouTubePreviews::changed, this, &Engine::previewChanged);

    pruneActivity();
    m_pruneTimer = new QTimer(this);
    m_pruneTimer->setInterval(kActivityPruneInterval);
    connect(m_pruneTimer, &QTimer::timeout, this, &Engine::pruneActivity);
    m_pruneTimer->start();
    m_budgetTimer = new QTimer(this);
    m_budgetTimer->setSingleShot(true);
    connect(m_budgetTimer, &QTimer::timeout, this, &Engine::reportSearchBudget);

    m_started = true;
    if (m_options.startPaused) {
        setPaused(true);
    }
    m_collection->setHistory(library::HistoryStore(*m_db).downloads());
    m_inbox->reconcile(); // publishes the Inbox, which the collection index takes in
    setLibraryConfig(m_options.library);
    m_meta->restore();
    m_network->start(m_options.network);
    m_transfers->load();
    m_getTracks->downloadsRestored(m_transfers->unfinished());
    Q_EMIT getLinesReset(m_getTracks->lines());
    qCInfo(lcEngine) << "engine started";
    Q_EMIT startFinished(true, QString());
    return true;
}

void Engine::stop()
{
    // Services go before the database they write to. Safe to call from the destructor at any stage of start().
    delete m_pruneTimer;
    m_pruneTimer = nullptr;
    delete m_budgetTimer;
    m_budgetTimer = nullptr;
    delete m_annotateTimer;
    m_annotateTimer = nullptr;
    m_manualSearch.reset(); // before the search and transfer services it uses
    m_getTracks.reset();
    m_collection.reset(); // cancels a scan in flight; what it read is read again next time
    m_arrival.reset();    // cancels conversions in flight (they run again at the next launch) and records finished ones
    m_search.reset();
    m_searchCache.reset();
    m_searchNetwork.reset();
    if (m_transfers) {
        m_transfers->shutdown(); // unfinished downloads stay saved; the manager closes their files as it goes
    }
    m_network.reset(); // before the token store it reads from (owned by m_meta), and after shutdown()
    m_transfers.reset();
    m_youtube.reset(); // uses m_meta's yt-dlp runner
    m_meta.reset();
    m_inbox.reset(); // waits for file work in flight and records it
    m_governorStore.reset();
    m_activity.reset();
    m_governor.reset();
    m_db.reset();
    if (m_started) {
        m_started = false;
        qCInfo(lcEngine) << "engine stopped";
    }
}

void Engine::setLibraryConfig(const LibraryConfig &config)
{
    if (!m_inbox || !m_collection) {
        return;
    }
    m_options.library = config;
    m_inbox->setConfig(config);
    // Selecta's own folders are never part of the collection, even when they sit inside it.
    QStringList excluded{config.inboxRoot};
    for (const QString &own : {m_options.incomingDir, m_options.stagingDir, m_options.previewDir}) {
        if (!own.isEmpty()) {
            excluded.append(own);
        }
    }
    m_collection->setRoots(config.collectionRoots, excluded);
}

void Engine::previewLine(qint64 lineId)
{
    if (!m_started) {
        return;
    }
    std::optional<GetLine> line;
    const QList<GetLine> lines = m_getTracks->lines();
    for (const GetLine &candidate : lines) {
        if (candidate.id == lineId) {
            line = candidate;
        }
    }
    if (!line) {
        return;
    }
    if (!line->best) {
        // No file on Soulseek yet: hear it from YouTube, through the user's own yt-dlp (ADR-011).
        stopPreview();
        meta::Release release;
        if (line->release) {
            release = *line->release;
        } else {
            release.title = line->title;
            release.artists = line->artist.isEmpty() ? QStringList() : QStringList{line->artist};
            release.durationMs = line->durationHintMs;
        }
        if (release.title.isEmpty()) {
            Q_EMIT previewRefused(previewKeyForLine(lineId), tr("There's nothing to look for in this line."));
            return;
        }
        m_previewId = m_youtube->start(lineId, release);
        return;
    }
    m_youtube->stop();
    const FileChoice &file = *line->best;
    DownloadSpec spec;
    spec.peer = file.peer;
    spec.remotePath = file.remotePath;
    spec.size = file.size;
    spec.file = file;
    spec.release = line->release;
    spec.getLine = lineId;
    QString why;
    const std::optional<TransferService::PreviewStart> started = m_transfers->startPreview(spec, &why);
    if (!started) {
        Q_EMIT previewRefused(previewKeyForLine(lineId), why);
        return;
    }
    m_previewId = started->id;
    const QString identity =
        u"peer|%1|%2|%3"_s.arg(file.peer, QString::fromLatin1(file.remotePath.toBase64())).arg(file.size);
    const QString title = line->release ? line->release->title : line->title;
    const QString artist = line->release ? line->release->artist() : line->artist;
    QString quality = file.extension.toUpper();
    if (file.bitDepth && file.sampleRate) {
        quality += u" %1/%2"_s.arg(*file.bitDepth).arg(*file.sampleRate / 1000.0, 0, 'g', 3);
    } else if (file.bitrate) {
        quality += u' ' + QString::number(*file.bitrate);
    }
    Q_EMIT previewStarted(previewKeyForLine(lineId), started->id, started->source, identity, title, artist,
                          tr("%1 from %2").arg(quality, file.peer));
}

void Engine::previewResult(qint64 searchId, int index)
{
    if (!m_started) {
        return;
    }
    const QString key = previewKeyForResult(searchId, index);
    const std::optional<SearchResult> result = m_manualSearch->result(searchId, index);
    if (!result || !result->isAudio()) {
        Q_EMIT previewRefused(key, tr("Only audio files can be previewed."));
        return;
    }
    m_youtube->stop();
    const std::optional<ManualSearch> search = m_manualSearch->search(searchId);
    const meta::Release *release = search ? search->chosenRelease() : nullptr;
    DownloadSpec spec;
    spec.peer = result->hit.peer;
    spec.remotePath = result->hit.path;
    spec.size = static_cast<qint64>(result->hit.size);
    spec.file = fileChoiceOf(*result);
    if (release && result->fitsRelease()) {
        spec.release = *release;
    }
    QString why;
    const std::optional<TransferService::PreviewStart> started = m_transfers->startPreview(spec, &why);
    if (!started) {
        Q_EMIT previewRefused(key, why);
        return;
    }
    m_previewId = started->id;
    const QString identity =
        u"peer|%1|%2|%3"_s.arg(spec.peer, QString::fromLatin1(spec.remotePath.toBase64())).arg(spec.size);
    QString title = result->parsed.title.value.isEmpty() ? result->name : result->parsed.title.value;
    QString artist = result->parsed.artist.value;
    if (spec.release) {
        title = spec.release->title;
        artist = spec.release->artist();
    }
    QString quality = result->hit.extension.toUpper();
    const match::AudioQuality &declared = result->quality.quality;
    if (declared.bitDepth > 0 && declared.sampleRateHz > 0) {
        quality += u" %1/%2"_s.arg(declared.bitDepth).arg(declared.sampleRateHz / 1000.0, 0, 'g', 3);
    } else if (declared.bitrateKbps > 0) {
        quality += u' ' + QString::number(declared.bitrateKbps);
    }
    Q_EMIT previewStarted(key, started->id, started->source, identity, title, artist,
                          tr("%1 from %2").arg(quality, spec.peer));
}

void Engine::openSearch(quint64 request, const QString &text)
{
    const qint64 id = m_started ? m_manualSearch->open(text) : 0;
    Q_EMIT searchOpened(request, id);
}

void Engine::annotate(QList<SearchResult> &results)
{
    if (!m_started) {
        return;
    }
    QSet<std::pair<QString, QByteArray>> queued;
    const QList<Download> unfinished = m_transfers->unfinished();
    for (const Download &download : unfinished) {
        queued.insert({download.peer, download.remotePath});
    }
    ResultAnnotator annotator(*m_db, *m_governor, std::move(queued), m_options.session);
    for (SearchResult &result : results) {
        annotator.annotate(result);
    }
}

void Engine::stopPreview()
{
    if (m_started && m_previewId != 0) {
        m_transfers->stopPreview(m_previewId);
        m_youtube->stop();
        m_previewId = 0;
    }
}

void Engine::addTracks(const QString &text)
{
    if (!m_started) {
        return;
    }
    const auto added = m_getTracks->add(text);
    const meta::spotify::PastedLinks links = meta::spotify::parsePastedLinks(text);
    QString note;
    if (!links.links.isEmpty()) {
        if (m_meta->spotify().isAvailable()) {
            note = links.links.size() == 1 ? tr("Importing the Spotify link…")
                                           : tr("Importing %1 Spotify links…").arg(links.links.size());
            m_meta->spotify().importLinks(links).then(this, [this](const meta::Result<meta::LinkImport> &result) {
                if (!m_started) {
                    return;
                }
                if (!result.ok()) {
                    Q_EMIT tracksAdded(0, 0, 0,
                                       tr("Spotify links couldn't be imported: %1").arg(result.error().message));
                    return;
                }
                const auto imported = m_getTracks->addReleases(result.value().tracks);
                QString message;
                if (!result.value().failed.isEmpty()) {
                    message = tr("%1 Spotify links couldn't be read.").arg(result.value().failed.size());
                }
                Q_EMIT tracksAdded(imported.added, imported.duplicates, 0, message);
            });
        } else {
            note = tr("Spotify links need Spotify set up in Settings; the other lines were added.");
        }
    }
    Q_EMIT tracksAdded(added.added, added.duplicates, added.unidentified, note);
}

void Engine::setPaused(bool paused)
{
    if (!m_governor || m_governor->isPaused() == paused) {
        return;
    }
    m_governor->setPaused(paused);
    m_activity->record(u"network"_s, QString(), paused ? u"paused"_s : u"resumed"_s,
                       paused ? tr("Network activity paused: no new searches or transfers.")
                              : tr("Network activity resumed."));
}

bool Engine::isPaused() const
{
    return m_governor && m_governor->isPaused();
}

void Engine::reportSearchBudget()
{
    if (!m_governor) {
        return;
    }
    const int available = m_governor->searchesAvailable();
    Q_EMIT searchBudgetChanged(available);
    if (available < governor::limits::kSearchBurst && m_budgetTimer) {
        // The next token's arrival, plus a millisecond so the bucket has really refilled when the timer fires.
        m_budgetTimer->start(m_governor->nextSearchRefillIn() + 1ms);
    }
}

void Engine::pruneActivity()
{
    if (m_activity) {
        const int removed = m_activity->prune();
        if (removed > 0) {
            qCInfo(lcEngine) << "pruned" << removed << "activity lines";
        }
    }
}

} // namespace selecta::core
