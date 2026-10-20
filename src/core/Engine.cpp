// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "base/Version.h"
#include "core/GovernorStore.h"
#include "core/ReleaseJson.h"
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
    connect(m_youtube.get(), &YouTubePreviews::started, this, &Engine::previewStarted);
    connect(m_youtube.get(), &YouTubePreviews::changed, this, &Engine::previewChanged);
    m_clips = std::make_unique<ClipPreviews>(m_meta->clips(), m_meta->network());
    connect(m_clips.get(), &ClipPreviews::started, this, &Engine::previewStarted);
    connect(m_clips.get(), &ClipPreviews::changed, this, &Engine::previewChanged);

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
    if (!m_options.ytDlpPath.isEmpty()) {
        m_meta->ytDlp().setExecutable(m_options.ytDlpPath);
    }
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
    m_clips.reset();   // and its clip source
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

namespace {

// "FLAC 16/44.1 from vinyl_ghost": what a peer preview plays.
QString peerPreviewLabel(const FileChoice &file)
{
    QString quality = file.extension.toUpper();
    if (file.bitDepth && file.sampleRate) {
        quality += u" %1/%2"_s.arg(*file.bitDepth).arg(*file.sampleRate / 1000.0, 0, 'g', 3);
    } else if (file.bitrate) {
        quality += u' ' + QString::number(*file.bitrate);
    }
    return Engine::tr("%1 from %2").arg(quality, file.peer);
}

} // namespace

void Engine::previewLine(qint64 lineId, PreviewSource source)
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
    PreviewPlan plan;
    plan.key = previewKeyForLine(lineId);
    meta::Release release;
    if (line->release) {
        release = *line->release;
    } else {
        release.title = line->title;
        release.artists = line->artist.isEmpty() ? QStringList() : QStringList{line->artist};
        release.durationMs = line->durationHintMs;
    }
    if (!release.title.isEmpty()) {
        plan.release = release;
    }
    if (line->best) {
        DownloadSpec spec;
        spec.peer = line->best->peer;
        spec.remotePath = line->best->remotePath;
        spec.size = line->best->size;
        spec.file = line->best;
        spec.release = line->release;
        spec.getLine = lineId;
        plan.peer = spec;
    }
    plan.title = line->release ? line->release->title : line->title;
    plan.artist = line->release ? line->release->artist() : line->artist;
    startPreview(std::move(plan), source);
}

void Engine::previewResult(qint64 searchId, int index, PreviewSource source)
{
    if (!m_started) {
        return;
    }
    const std::optional<SearchResult> result = m_manualSearch->result(searchId, index);
    if (!result || !result->isAudio()) {
        Q_EMIT previewRefused(previewKeyForResult(searchId, index), tr("Only audio files can be previewed."));
        return;
    }
    const std::optional<ManualSearch> search = m_manualSearch->search(searchId);
    const meta::Release *chosen = search ? search->chosenRelease() : nullptr;
    PreviewPlan plan;
    plan.key = previewKeyForResult(searchId, index);
    DownloadSpec spec;
    spec.peer = result->hit.peer;
    spec.remotePath = result->hit.path;
    spec.size = static_cast<qint64>(result->hit.size);
    spec.file = fileChoiceOf(*result);
    if (chosen && result->fitsRelease()) {
        spec.release = *chosen;
        plan.release = *chosen;
    } else if (!result->parsed.title.value.isEmpty()) {
        // The file's own track, as its name says.
        meta::Release own;
        own.title = result->parsed.title.value;
        if (!result->parsed.artist.value.isEmpty()) {
            own.artists = {result->parsed.artist.value};
        }
        if (result->durationSec() > 0) {
            own.durationMs = qint64{result->durationSec()} * 1000;
        }
        plan.release = own;
    }
    plan.peer = spec;
    plan.title = plan.release ? plan.release->title : result->name;
    plan.artist = plan.release ? plan.release->artist() : QString();
    startPreview(std::move(plan), source);
}

void Engine::startPreview(PreviewPlan plan, PreviewSource source)
{
    stopPreview();
    // D26: Space hears the track from the user's yt-dlp, else Deezer's clip; the peer's file is the last resort and
    // what ⇧Space asks for.
    QList<PreviewSource> attempts;
    switch (source) {
    case PreviewSource::Automatic:
        attempts = {PreviewSource::YouTube, PreviewSource::Clip, PreviewSource::Peer};
        break;
    default:
        attempts = {source};
        break;
    }
    tryPreview(std::make_shared<const PreviewPlan>(std::move(plan)), attempts, 0, m_previewGeneration, {});
}

void Engine::tryPreview(const std::shared_ptr<const PreviewPlan> &plan, const QList<PreviewSource> &attempts,
                        qsizetype next, quint64 generation, QString lastError)
{
    if (!m_started || generation != m_previewGeneration) {
        return; // another preview was asked for meanwhile
    }
    const auto fallBack = [this, plan, attempts, generation](qsizetype after) {
        return [this, plan, attempts, generation, after](const QString &why) {
            tryPreview(plan, attempts, after + 1, generation, why);
        };
    };
    for (; next < attempts.size(); ++next) {
        switch (attempts.at(next)) {
        case PreviewSource::YouTube:
            if (plan->release) {
                m_previewId = m_youtube->start(plan->key, *plan->release, fallBack(next));
                return;
            }
            break;
        case PreviewSource::Clip:
            if (plan->release) {
                m_previewId = m_clips->start(plan->key, *plan->release, fallBack(next));
                return;
            }
            break;
        case PreviewSource::Peer: {
            if (!plan->peer) {
                lastError = tr("No file on Soulseek to preview yet.");
                break;
            }
            QString why;
            const std::optional<TransferService::PreviewStart> started = m_transfers->startPreview(*plan->peer, &why);
            if (!started) {
                lastError = why;
                break;
            }
            m_previewId = started->id;
            const DownloadSpec &spec = *plan->peer;
            const QString identity =
                u"peer|%1|%2|%3"_s.arg(spec.peer, QString::fromLatin1(spec.remotePath.toBase64())).arg(spec.size);
            Q_EMIT previewStarted(plan->key, started->id, started->source, identity, plan->title, plan->artist,
                                  spec.file ? peerPreviewLabel(*spec.file) : tr("From %1").arg(spec.peer));
            return;
        }
        case PreviewSource::Automatic:
            break;
        }
    }
    Q_EMIT previewRefused(plan->key, lastError.isEmpty() ? tr("There's nothing to preview this from.") : lastError);
}

void Engine::openSearch(quint64 request, const QString &text)
{
    const qint64 id = m_started ? m_manualSearch->open(text) : 0;
    Q_EMIT searchOpened(request, id);
}

void Engine::setYtDlpPath(const QString &path)
{
    if (m_started) {
        m_meta->ytDlp().setExecutable(path);
    }
}

void Engine::checkYtDlp()
{
    if (!m_started) {
        return;
    }
    m_meta->ytDlp().probe().then(this, [this](const meta::Result<meta::YtDlpInstall> &result) {
        if (!m_started) {
            return;
        }
        if (!result.ok()) {
            Q_EMIT ytDlpChecked(false, result.error().message);
            return;
        }
        const meta::YtDlpInstall &install = result.value();
        QString text = tr("yt-dlp %1 at %2").arg(install.version, install.path);
        if (install.denoPath.isEmpty()) {
            text += u". "_s + tr("Deno isn't installed, and YouTube needs it: brew install deno");
        } else if (install.stale) {
            text += u". "_s + tr("It's getting old: brew upgrade yt-dlp");
        }
        Q_EMIT ytDlpChecked(true, text);
    });
}

void Engine::editInboxTags(qint64 itemId, const convert::MetadataModel &changes, const QStringList &cleared)
{
    if (m_started) {
        m_inbox->retag(itemId, changes, cleared, u"user"_s);
    }
}

void Engine::findInboxReleases(qint64 itemId)
{
    if (!m_started) {
        return;
    }
    const std::optional<library::InboxItem> item = m_inbox->item(itemId);
    if (!item) {
        return;
    }
    meta::ResolveQuery query;
    query.artist = item->artist;
    query.title = item->version.isEmpty() || item->title.contains(item->version)
                      ? item->title
                      : u"%1 (%2)"_s.arg(item->title, item->version);
    query.album = item->album;
    if (item->durationMs > 0) {
        query.durationMs = item->durationMs;
    }
    m_meta->resolver()
        .resolve(query, meta::Priority::Interactive)
        .then(this, [this, itemId](const meta::Result<meta::Resolution> &result) {
            if (!m_started) {
                return;
            }
            if (!result.ok()) {
                Q_EMIT inboxReleasesFound(itemId, {}, result.error().message);
                return;
            }
            Q_EMIT inboxReleasesFound(itemId, result.value().candidates,
                                      result.value().candidates.isEmpty() ? tr("No release matches it.") : QString());
        });
}

void Engine::applyInboxRelease(qint64 itemId, const meta::Release &release)
{
    if (!m_started) {
        return;
    }
    // As an arrival does (D30): label and genre from MusicBrainz by ISRC when the release lacks them, then the cover.
    const auto retag = [this, itemId](const meta::Release &complete, const QByteArray &cover) {
        convert::MetadataModel tags = tagsFromRelease(complete);
        if (!cover.isEmpty()) {
            convert::Picture picture;
            picture.data = cover;
            picture.provenance = convert::Provenance::Release;
            tags.setFrontCover(picture);
        }
        m_inbox->retag(itemId, tags, {}, tagSourceOf(complete), QString::fromUtf8(releaseToText(complete)));
    };
    const auto withCover = [this, retag](const meta::Release &complete) {
        meta::CoverFetcher *covers = m_meta->covers();
        if (!covers) {
            retag(complete, {});
            return;
        }
        covers->fetch(complete, meta::Priority::Interactive)
            .then(this, [this, retag, complete](const meta::Result<meta::CoverImage> &cover) {
                if (m_started) {
                    retag(complete, cover.ok() ? cover.value().bytes : QByteArray());
                }
            });
    };
    if (release.isrc.isEmpty() || (!release.label.isEmpty() && !release.genres.isEmpty())) {
        withCover(release);
        return;
    }
    m_meta->musicBrainz()
        .enrich(release, meta::Priority::Interactive)
        .then(this, [this, withCover, release](const meta::Result<meta::musicbrainz::Enrichment> &result) {
            if (m_started) {
                withCover(result.ok() ? meta::MusicBrainzClient::applied(release, result.value()) : release);
            }
        });
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
    ++m_previewGeneration; // a fallback still on its way doesn't start anything
    if (m_started && m_previewId != 0) {
        m_transfers->stopPreview(m_previewId);
        m_youtube->stop();
        m_clips->stop();
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
