// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Engine.h"

#include "base/Clock.h"
#include "base/Version.h"
#include "core/GovernorStore.h"
#include "core/MatchCorrections.h"
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
    quint32 searchWishlist(const QString &query, governor::Permit permit) override
    {
        return m_network.searchWishlist(query, std::move(permit));
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
    metaOptions.spotifyAllowed = m_options.spotifyAllowed;
    m_meta = std::make_unique<MetaServices>(*m_governor, *m_activity, *m_clock, std::move(metaOptions));
    connect(m_meta.get(), &MetaServices::spotifyStatusChanged, this, &Engine::spotifyStatusChanged);

    m_network = std::make_unique<NetworkService>(*m_governor, *m_activity, m_meta->tokens(), m_options.wallClock);
    if (m_options.portMapping) {
        m_network->enablePortMapping(*m_options.portMapping);
    }
    connect(m_network.get(), &NetworkService::statusChanged, this, &Engine::networkStatusChanged);

    m_searchNetwork = std::make_unique<NetworkSearch>(*m_network);
    m_search = std::make_unique<SearchService>(*m_governor, *m_searchNetwork, *m_clock);
    m_searchCache = std::make_unique<SearchCacheStore>(*m_db, m_options.wallClock);
    m_search->setStore(m_searchCache.get());
    connect(m_network.get(), &NetworkService::searchResponseReceived, m_search.get(), &SearchService::received);
    connect(m_network.get(), &NetworkService::statusChanged, m_search.get(), &SearchService::networkChanged);
    connect(m_governor.get(), &governor::Governor::pausedChanged, m_search.get(), &SearchService::networkChanged);
    connect(m_search.get(), &SearchService::queueChanged, this, &Engine::searchQueueChanged);
    if (m_options.countries) {
        // Peers' flags (P-002): the country database, downloaded once when it isn't on disk yet.
        m_countries = std::make_unique<CountryDatabaseFetcher>(*m_options.countries, m_meta->network());
        connect(m_countries.get(), &CountryDatabaseFetcher::ready, m_search.get(), &SearchService::setCountries);
        m_countries->start();
    }

    m_getTracks = std::make_unique<GetTracksService>(*m_db, m_options.wallClock);
    m_getTracks->setSearch(m_search.get(), [this] { return m_governor->excludedPhrases(); });
    m_getTracks->setPeerLoad(
        [governor = m_governor.get()](const QString &peer) { return governor->peerToday(peer).files; });
    // Release lookups for a whole list go in the background lane: previews the user waits for overtake them.
    m_getTracks->setResolver([resolver = &m_meta->resolver()](const meta::ResolveQuery &query) {
        return resolver->resolve(query, meta::Priority::Background);
    });
    connect(m_getTracks.get(), &GetTracksService::linesReset, this, &Engine::getLinesReset);
    connect(m_getTracks.get(), &GetTracksService::lineChanged, this, &Engine::getLineChanged);

    m_inbox = std::make_unique<InboxService>(*m_db, m_options.library, m_options.trash);
    connect(m_inbox.get(), &InboxService::changed, this, &Engine::inboxChanged);
    connect(m_inbox.get(), &InboxService::finished, this, &Engine::inboxOperationFinished);
    connect(m_inbox.get(), &InboxService::finished, this, [this](qint64 id, bool ok) {
        if (!ok) {
            m_userTrashed.remove(id);
        }
    });
    connect(m_inbox.get(), &InboxService::trashed, this, [this](const TrashedItem &item) {
        if (m_userTrashed.remove(item.item.id)) {
            Q_EMIT inboxItemTrashed(item);
        }
    });

    // Downloads, then the arrival pipeline that turns each complete file into an Inbox item (D27, D29).
    // The preview cache (PREV-8, CACHE-1): what was previewed from peers and YouTube, kept across launches within
    // its bound. Opening it deletes what a quit or crash left half-written.
    if (!m_options.previewDir.isEmpty()) {
        PreviewCache::Options cacheOptions;
        cacheOptions.now = m_options.wallClock;
        m_previewCache = std::make_unique<PreviewCache>(m_options.previewDir, std::move(cacheOptions));
    }
    TransferService::Options transferOptions;
    transferOptions.incomingDir = m_options.incomingDir;
    transferOptions.previewCache = m_previewCache.get();
    m_transfers = std::make_unique<TransferService>(*m_db, *m_governor, *m_network, *m_activity,
                                                    std::move(transferOptions), m_options.wallClock);
    ArrivalService::Options arrivalOptions;
    arrivalOptions.stagingDir = m_options.stagingDir;
    arrivalOptions.encodedBy = u"Selecta "_s + QString(base::kVersion);
    arrivalOptions.session = m_options.session;
    arrivalOptions.analysisCache = m_options.analysisCache;
    m_arrival = std::make_unique<ArrivalService>(*m_db, *m_inbox, *m_activity, m_options.trash,
                                                 std::move(arrivalOptions), m_options.wallClock);
    m_arrival->setMeta(m_meta.get());
    AlbumService::Options albumOptions;
    albumOptions.stagingDir = m_options.stagingDir;
    albumOptions.encodedBy = u"Selecta "_s + QString(base::kVersion);
    albumOptions.session = m_options.session;
    m_albums = std::make_unique<AlbumService>(*m_db, *m_governor, *m_network, *m_transfers, *m_inbox, *m_activity,
                                              m_options.trash, std::move(albumOptions), m_options.wallClock);
    m_albums->setMeta(m_meta.get());
    // A file of an album waits for the album's other files; any other file arrives on its own.
    connect(m_transfers.get(), &TransferService::completed, this, [this](const Download &download) {
        if (m_albums && m_albums->fileCompleted(download)) {
            return;
        }
        if (m_arrival) {
            m_arrival->start(download);
        }
    });
    connect(m_transfers.get(), &TransferService::changed, m_albums.get(), &AlbumService::downloadChanged);
    connect(m_inbox.get(), &InboxService::trashed, m_albums.get(), &AlbumService::inboxItemTrashed);
    connect(m_albums.get(), &AlbumService::albumChanged, this, &Engine::albumChanged);
    connect(m_albums.get(), &AlbumService::flagsChanged, this, &Engine::refreshSupersedeFlags);
    connect(m_inbox.get(), &InboxService::finished, this, [this](qint64 /*id*/, bool ok) {
        if (ok) {
            refreshSupersedeFlags(); // a flagged single filed or trashed takes its flag along
        }
    });
    connect(m_albums.get(), &AlbumService::reported, this, &Engine::albumReported);
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
    m_manualSearch->setAlbumDownloads([albums = m_albums.get()](const QString &peer, const QByteArray &folder,
                                                                const QList<SearchHit> &known,
                                                                const std::optional<meta::Release> &release) {
        return albums->download(peer, folder, known, release).has_value();
    });
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
    // Sharing: the collection, read-only (D12).
    m_shares = std::make_unique<ShareService>(*m_db, *m_governor, *m_network, *m_activity, *m_clock, m_options.session,
                                              m_options.wallClock);
    connect(m_shares.get(), &ShareService::statusChanged, this, &Engine::shareStatusChanged);
    connect(m_shares.get(), &ShareService::uploadsChanged, this, &Engine::uploadsChanged);
    connect(m_collection.get(), &CollectionService::indexChanged, this,
            [this](const std::shared_ptr<const match::MatchIndex> &index) {
                m_getTracks->setLibraryIndex(index);
                m_manualSearch->setLibraryIndex(index);
                if (m_browse) {
                    m_browse->setLibraryIndex(index);
                }
                m_shares->collectionChanged();
            });
    connect(m_arrival.get(), &ArrivalService::finished, m_shares.get(), &ShareService::refreshTotals);
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
    connect(m_albums.get(), &AlbumService::albumChanged, this, [this](const AlbumDownload &album) {
        if (album.state == AlbumDownload::State::Arrived || album.state == AlbumDownload::State::Partial) {
            m_shares->refreshTotals();
            m_collection->setHistory(library::HistoryStore(*m_db).downloads());
        }
    });
    connect(m_transfers.get(), &TransferService::changed, m_getTracks.get(), &GetTracksService::downloadChanged);
    connect(m_transfers.get(), &TransferService::progress, m_getTracks.get(), &GetTracksService::downloadProgress);
    connect(m_transfers.get(), &TransferService::previewChanged, this, &Engine::previewChanged);
    connect(m_transfers.get(), &TransferService::changed, this, &Engine::downloadChanged);
    connect(m_transfers.get(), &TransferService::progress, this, &Engine::downloadProgress);
    m_peerStatsTimer = new QTimer(this);
    m_peerStatsTimer->setSingleShot(true);
    m_peerStatsTimer->setInterval(1s);
    connect(m_peerStatsTimer, &QTimer::timeout, this, &Engine::refreshPeerStats);
    connect(m_transfers.get(), &TransferService::changed, m_peerStatsTimer, qOverload<>(&QTimer::start));
    // Private messages and the user lists (NET-7, NET-8); banned users get nothing of our shares.
    m_social = std::make_unique<SocialService>(*m_db, *m_governor, *m_network, *m_activity, m_options.wallClock);
    connect(m_social.get(), &SocialService::conversationsChanged, this, &Engine::conversationsChanged);
    connect(m_social.get(), &SocialService::messageAdded, this, &Engine::messageAdded);
    connect(m_social.get(), &SocialService::listsChanged, this, &Engine::userListsChanged);
    connect(m_social.get(), &SocialService::bannedChanged, m_shares.get(), &ShareService::setBanned);
    m_shares->setBanned(m_social->banned());
    // Chat rooms (NET-10).
    m_rooms =
        std::make_unique<RoomService>(*m_db, *m_governor, *m_network, *m_social, *m_activity, m_options.wallClock);
    connect(m_rooms.get(), &RoomService::listChanged, this, &Engine::roomListChanged);
    connect(m_rooms.get(), &RoomService::roomChanged, this, &Engine::roomChanged);
    connect(m_rooms.get(), &RoomService::roomClosed, this, &Engine::roomClosed);
    connect(m_rooms.get(), &RoomService::lineAdded, this, &Engine::roomLineAdded);

    // Browsing peers' shares (BRWS-1, BRWS-2).
    m_browse = std::make_unique<BrowseService>(*m_db, *m_governor, *m_network, m_options.wallClock);
    m_browse->setResolver([resolver = &m_meta->resolver()](const meta::ResolveQuery &query) {
        return resolver->resolve(query, meta::Priority::Interactive);
    });
    m_browse->setAnnotate([this](QList<SearchResult> &results) { annotate(results); });
    m_browse->setDownloads(
        [transfers = m_transfers.get()](const DownloadSpec &spec) { return transfers->enqueue(spec).has_value(); },
        [albums = m_albums.get()](const QString &peer, const QByteArray &folder, const QList<SearchHit> &files,
                                  const std::optional<meta::Release> &release) {
            return albums->download(peer, folder, files, release, true).has_value();
        });
    connect(&m_network->peers(), &slsk::PeerNetwork::sharedFileListReceived, m_browse.get(),
            &BrowseService::listReceived);
    connect(&m_network->peers(), &slsk::PeerNetwork::sharedFileListProgress, m_browse.get(),
            &BrowseService::listProgress);
    connect(&m_network->peers(), &slsk::PeerNetwork::requestFailed, m_browse.get(),
            [browse = m_browse.get()](const QString &peer, slsk::PeerRequest request, quint32 /*token*/,
                                      slsk::PeerError error) {
                if (request == slsk::PeerRequest::SharedFileList) {
                    browse->requestFailed(peer, describePeerError(peer, error));
                }
            });
    connect(m_browse.get(), &BrowseService::browseChanged, this, &Engine::browseChanged);
    connect(m_browse.get(), &BrowseService::browseClosed, this, &Engine::browseClosed);
    connect(m_browse.get(), &BrowseService::resultsReset, this, &Engine::browseResultsReset);

    // The wishlist (SRCH-14): saved searches run again at the server's interval.
    m_wishlist = std::make_unique<WishlistService>(*m_db, *m_search, m_options.wallClock);
    connect(m_wishlist.get(), &WishlistService::itemsChanged, this, &Engine::wishlistChanged);
    connect(m_wishlist.get(), &WishlistService::found, this, &Engine::wishlistFound);
    connect(m_network.get(), &NetworkService::wishlistIntervalChanged, m_wishlist.get(),
            [wishlist = m_wishlist.get()](std::chrono::seconds interval) { wishlist->setInterval(interval); });

    // The manual converter (CONV-3): new files next to the originals or in a chosen folder.
    ConverterService::Options converterOptions;
    converterOptions.stagingDir = m_options.stagingDir;
    converterOptions.encodedBy = u"Selecta "_s + QString(base::kVersion);
    m_converter = std::make_unique<ConverterService>(std::move(converterOptions));
    connect(m_converter.get(), &ConverterService::itemsChanged, this, &Engine::converterChanged);
    connect(m_converter.get(), &ConverterService::finished, this, &Engine::conversionsFinished);
    m_youtube = std::make_unique<YouTubePreviews>(m_meta->ytDlp(), m_previewCache.get());
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
    m_albums->restore();
    m_getTracks->downloadsRestored(m_transfers->unfinished());
    Q_EMIT downloadsReset(m_transfers->unfinished());
    refreshPeerStats();
    refreshSupersedeFlags();
    Q_EMIT wishlistChanged(m_wishlist->items());
    Q_EMIT conversationsChanged(m_social->conversations());
    Q_EMIT userListsChanged(m_social->listed());
    const QList<Room> rooms = m_rooms->rooms();
    for (const Room &room : rooms) {
        Q_EMIT roomChanged(room);
    }
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
    delete m_peerStatsTimer;
    m_peerStatsTimer = nullptr;
    m_rooms.reset();
    m_social.reset();
    m_browse.reset();       // waits for an analysis in flight
    m_wishlist.reset();     // cancels its search in flight
    m_manualSearch.reset(); // before the search and transfer services it uses
    m_getTracks.reset();
    m_shares.reset();     // stops uploads under way (their downloaders queue again later)
    m_collection.reset(); // cancels a scan in flight; what it read is read again next time
    m_converter.reset();  // cancels conversions in flight: nothing half-made is left
    m_albums.reset();     // cancels albums being made (they are made again at the next launch)
    m_arrival.reset();    // cancels conversions in flight (they run again at the next launch) and records finished ones
    m_countries.reset();  // waits for its worker; before the network manager it uses (owned by m_meta)
    m_search.reset();
    m_searchCache.reset();
    m_searchNetwork.reset();
    if (m_transfers) {
        m_transfers->shutdown(); // unfinished downloads stay saved; the manager closes their files as it goes
    }
    m_network.reset(); // before the token store it reads from (owned by m_meta), and after shutdown()
    m_transfers.reset();
    m_youtube.reset();      // uses m_meta's yt-dlp runner
    m_clips.reset();        // and its clip source
    m_previewCache.reset(); // after the transfers and the YouTube previews that hold files in it
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

void Engine::trashInboxItems(const QList<qint64> &ids)
{
    if (!m_inbox) {
        return;
    }
    for (const qint64 id : ids) {
        m_userTrashed.insert(id);
    }
    m_inbox->trash(ids);
}

void Engine::restoreInboxItems(const QList<TrashedItem> &items)
{
    if (!m_inbox) {
        return;
    }
    for (const TrashedItem &item : items) {
        m_inbox->restore(item);
    }
}

void Engine::cancelDownloads(const QList<qint64> &ids)
{
    if (!m_transfers) {
        return;
    }
    for (const qint64 id : ids) {
        m_transfers->cancel(id);
    }
}

void Engine::retryDownloads(const QList<qint64> &ids)
{
    if (!m_transfers) {
        return;
    }
    for (const qint64 id : ids) {
        m_transfers->retry(id);
    }
}

void Engine::refreshPeerStats()
{
    if (!m_transfers || !m_governor) {
        return;
    }
    QSet<QString> current;
    const QList<Download> unfinished = m_transfers->unfinished();
    for (const Download &download : unfinished) {
        current.insert(download.peer);
    }
    const std::optional<qint64> session = m_options.session ? std::optional<qint64>(*m_options.session) : std::nullopt;
    Q_EMIT peerStatsChanged(peerStats(library::HistoryStore(*m_db).peerTotals(session), current, *m_governor));
}

void Engine::refreshSupersedeFlags()
{
    if (m_db) {
        Q_EMIT supersedeFlagsChanged(SupersedeFlagStore(*m_db).all());
    }
}

void Engine::correctSearchMatch(qint64 searchId, int index, bool same)
{
    if (!m_started) {
        return;
    }
    const std::optional<SearchResult> result = m_manualSearch->result(searchId, index);
    if (!result) {
        return;
    }
    if (!ResultAnnotator::recordCorrection(*m_db, *result, same ? Verdict::Same : Verdict::Different,
                                           m_options.wallClock())) {
        qCWarning(lcEngine) << "couldn't remember the correction of" << result->path << ':' << m_db->lastError();
        return;
    }
    refreshCorrections();
}

void Engine::notOwnedFindTracks(const QList<qint64> &ids)
{
    if (m_started) {
        m_getTracks->notOwned(ids);
        refreshCorrections();
    }
}

void Engine::notTheSameTrack(qint64 flagId)
{
    if (!m_db) {
        return;
    }
    SupersedeFlagStore flags(*m_db);
    const std::optional<SupersedeFlag> flag = flags.find(flagId);
    if (!flag) {
        return;
    }
    // Flags from before LIB-6 don't know their album track: they can only go.
    if (flag->singleTrack.isValid() && flag->albumTrack.isValid() &&
        !MatchCorrectionStore(*m_db).record(CorrectionKind::Supersede, flag->singleTrack, flag->albumTrack,
                                            Verdict::Different, m_options.wallClock())) {
        qCWarning(lcEngine) << "couldn't remember that" << flag->single << "isn't on" << flag->album << ':'
                            << m_db->lastError();
        return;
    }
    dismissSupersedeFlag(flagId); // a supersede verdict holds for supersede only: nothing else to look at again
}

void Engine::refreshCorrections()
{
    // A verdict on a wanted track and an owned copy holds for search results and Get tracks lines alike.
    m_manualSearch->refreshAnnotations();
    if (m_browse) {
        m_browse->refreshAnnotations();
    }
    m_getTracks->recheckOwnership();
}

void Engine::dismissSupersedeFlag(qint64 id)
{
    if (!m_db) {
        return;
    }
    if (!SupersedeFlagStore(*m_db).remove(id)) {
        qCWarning(lcEngine) << "couldn't dismiss flag" << id << ':' << m_db->lastError();
    }
    refreshSupersedeFlags();
    if (m_inbox) {
        m_inbox->refresh();
    }
}

void Engine::sendMessage(const QString &peer, const QString &text)
{
    if (m_social) {
        Q_EMIT messageSent(peer, m_social->send(peer, text));
    }
}

void Engine::joinRoom(const QString &room)
{
    if (m_rooms) {
        if (const QString problem = m_rooms->join(room); !problem.isEmpty()) {
            Q_EMIT roomRefused(room, problem);
        }
    }
}

void Engine::sayInRoom(const QString &room, const QString &text)
{
    if (m_rooms) {
        Q_EMIT roomSaid(room, m_rooms->say(room, text));
    }
}

void Engine::loadConversation(const QString &peer)
{
    if (m_social) {
        Q_EMIT conversationLoaded(peer, m_social->messages(peer));
    }
}

void Engine::refreshCaches()
{
    if (!m_searchCache || !m_transfers) {
        return;
    }
    CacheStatus status;
    status.searches = m_searchCache->count();
    status.searchBytes = m_searchCache->sizeBytes();
    status.previewBytes = m_previewCache ? m_previewCache->totalBytes() : 0;
    Q_EMIT cachesChanged(status);
}

void Engine::clearSearchCache()
{
    if (m_searchCache && !m_searchCache->clear()) {
        qCWarning(lcEngine) << "couldn't clear the search cache:" << m_db->lastError();
    }
    refreshCaches();
}

void Engine::clearPreviewCache()
{
    if (m_previewCache) {
        m_previewCache->clear(); // a preview playing stays
    }
    refreshCaches();
}

void Engine::setLibraryConfig(const LibraryConfig &config)
{
    if (!m_inbox || !m_collection || !m_shares) {
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
    const collection::FolderRoles folders = config.folderRoles();
    m_collection->setFolders(folders, excluded);
    m_shares->setRoots(folders.roots(), excluded); // shared whole, whatever their roles
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

void Engine::previewBrowsed(const QString &peer, int index, PreviewSource source)
{
    if (!m_started) {
        return;
    }
    const QList<SearchResult> results = m_browse->results(peer);
    if (index < 0 || index >= results.size() || !results.at(index).isAudio()) {
        Q_EMIT previewRefused(previewKeyForBrowsed(peer, index), tr("Only audio files can be previewed."));
        return;
    }
    startPreview(planFor(results.at(index), previewKeyForBrowsed(peer, index), nullptr), source);
}

Engine::PreviewPlan Engine::planFor(const SearchResult &result, const QString &key, const meta::Release *chosen)
{
    PreviewPlan plan;
    plan.key = key;
    DownloadSpec spec;
    spec.peer = result.hit.peer;
    spec.remotePath = result.hit.path;
    spec.size = static_cast<qint64>(result.hit.size);
    spec.file = fileChoiceOf(result);
    if (chosen && result.fitsRelease()) {
        spec.release = *chosen;
        plan.release = *chosen;
    } else if (!result.parsed.title.value.isEmpty()) {
        // The file's own track, as its name says.
        meta::Release own;
        own.title = result.parsed.title.value;
        if (!result.parsed.artist.value.isEmpty()) {
            own.artists = {result.parsed.artist.value};
        }
        if (result.durationSec() > 0) {
            own.durationMs = qint64{result.durationSec()} * 1000;
        }
        plan.release = own;
    }
    plan.peer = spec;
    plan.title = plan.release ? plan.release->title : result.name;
    plan.artist = plan.release ? plan.release->artist() : QString();
    return plan;
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
    startPreview(planFor(*result, previewKeyForResult(searchId, index), chosen), source);
}

void Engine::startPreview(PreviewPlan plan, PreviewSource source)
{
    stopPreview();
    // D26: Space hears the track from the user's yt-dlp, else Deezer's clip; the peer's file is the last resort and
    // what ⇧Space asks for.
    const QList<PreviewSource> attempts =
        source == PreviewSource::Automatic
            ? QList<PreviewSource>{PreviewSource::YouTube, PreviewSource::Clip, PreviewSource::Peer}
            : QList<PreviewSource>{source};
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

void Engine::discardPreview(qint64 previewId)
{
    if (!m_started || previewId == 0) {
        return;
    }
    if (previewId == m_previewId) {
        ++m_previewGeneration; // a fallback still on its way doesn't start anything
        m_clips->stop();       // a clip lives in memory only: it just stops
        m_previewId = 0;
    }
    if (!m_transfers->discardPreview(previewId)) {
        m_youtube->discard(previewId);
    }
    refreshCaches(); // Settings shows the cache without it
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
