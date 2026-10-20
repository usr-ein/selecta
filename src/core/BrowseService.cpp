// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/BrowseService.h"

#include "core/NetworkService.h"
#include "governor/Governor.h"
#include "slsk/Codec.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QLoggingCategory>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcBrowse, "selecta.core.browse")

using storage::Statement;
using State = Browse::State;

constexpr qint64 kMaxCachedBytes = qint64{64} * 1024 * 1024;

QByteArray encodeList(const slsk::SharedFileList &list)
{
    slsk::Writer writer;
    for (const QList<slsk::SharedDirectory> *directories : {&list.directories, &list.privateDirectories}) {
        writer.u32(static_cast<quint32>(directories->size()));
        for (const slsk::SharedDirectory &directory : *directories) {
            directory.encode(writer);
        }
    }
    return qCompress(writer.data());
}

std::optional<slsk::SharedFileList> decodeList(const QByteArray &blob)
{
    const QByteArray bytes = qUncompress(blob);
    if (bytes.isEmpty()) {
        return std::nullopt;
    }
    slsk::Reader reader(bytes);
    slsk::SharedFileList list;
    for (QList<slsk::SharedDirectory> *directories : {&list.directories, &list.privateDirectories}) {
        const qsizetype count = reader.count(slsk::SharedDirectory::kMinBytes);
        directories->reserve(count);
        for (qsizetype i = 0; i < count && reader.ok(); ++i) {
            std::optional<slsk::SharedDirectory> directory = slsk::SharedDirectory::decode(reader);
            if (!directory) {
                return std::nullopt;
            }
            directories->append(std::move(*directory));
        }
    }
    return reader.ok() ? std::optional(std::move(list)) : std::nullopt;
}

// Every file of the list as a search hit: the full path, the peer's attributes.
QList<SearchHit> hitsOf(const QString &peer, const slsk::SharedFileList &list)
{
    QList<SearchHit> hits;
    for (const QList<slsk::SharedDirectory> *directories : {&list.directories, &list.privateDirectories}) {
        for (const slsk::SharedDirectory &directory : *directories) {
            for (const slsk::FileEntry &file : directory.files) {
                SearchHit hit;
                hit.peer = peer;
                hit.path = directory.path.raw + '\\' + file.name.raw;
                hit.size = file.size;
                hit.extension = file.extension.toLower();
                hit.attributes = file.attributes;
                hits.append(hit);
            }
        }
    }
    return hits;
}

} // namespace

QString describePeerError(const QString &peer, slsk::PeerError error)
{
    switch (error) {
    case slsk::PeerError::UserOffline:
        return BrowseService::tr("%1 is offline.").arg(peer);
    case slsk::PeerError::Unreachable:
        return BrowseService::tr("%1 couldn't be reached: their connection may be closed to others.").arg(peer);
    case slsk::PeerError::Timeout:
        return BrowseService::tr("%1 didn't answer in time.").arg(peer);
    case slsk::PeerError::ConnectionClosed:
        return BrowseService::tr("%1 closed the connection before answering.").arg(peer);
    case slsk::PeerError::ProtocolError:
        return BrowseService::tr("%1's answer couldn't be read.").arg(peer);
    case slsk::PeerError::NotLoggedIn:
        return BrowseService::tr("Not connected to Soulseek.");
    case slsk::PeerError::NotPermitted:
    case slsk::PeerError::SocketLimit:
        return BrowseService::tr("Selecta holds back connections right now; try again in a moment.");
    case slsk::PeerError::Cancelled:
        return BrowseService::tr("Cancelled.");
    case slsk::PeerError::None:
        break;
    }
    return BrowseService::tr("%1 couldn't be asked.").arg(peer);
}

BrowseService::BrowseService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                             WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
    qRegisterMetaType<Browse>();
    m_pool.setMaxThreadCount(1);
    m_pool.setObjectName(u"selecta-browse"_s);
}

BrowseService::~BrowseService()
{
    m_pool.waitForDone();
}

void BrowseService::setLibraryIndex(std::shared_ptr<const match::MatchIndex> index)
{
    m_index = std::move(index);
}

void BrowseService::setDownloads(Enqueue enqueue, EnqueueAlbum enqueueAlbum)
{
    m_enqueue = std::move(enqueue);
    m_enqueueAlbum = std::move(enqueueAlbum);
}

void BrowseService::setState(Browse &browse, Browse::State state, const QString &message)
{
    browse.state = state;
    browse.message = message;
    Q_EMIT browseChanged(browse);
}

void BrowseService::browse(const QString &peer, bool refresh)
{
    if (peer.isEmpty()) {
        return;
    }
    Browse &entry = m_browses[peer];
    entry.peer = peer;
    if (!refresh) {
        if (const auto kept = cached(peer)) {
            accept(peer, kept->first, kept->second, true);
            return;
        }
    }
    // A browse costs the peer a whole share list: through the governor's budget (SAFE-1, BRWS-2).
    governor::Decision decision = m_governor.request(governor::Operation::Browse, {.peer = peer});
    if (!decision.permit) {
        setState(entry, entry.state == State::Ready ? State::Ready : State::Failed, decision.explanation);
        return;
    }
    if (!m_network.isOnline() || !m_network.peers().requestSharedFileList(peer, std::move(*decision.permit))) {
        setState(entry, State::Failed, tr("Not connected to Soulseek."));
        return;
    }
    entry.received = 0;
    entry.total = 0;
    setState(entry, State::Requesting, tr("Asking %1 for the list of their shared files").arg(peer));
}

void BrowseService::close(const QString &peer)
{
    m_browses.remove(peer);
    m_results.remove(peer);
    ++m_generation[peer]; // an analysis in flight is dropped
    Q_EMIT browseClosed(peer);
}

std::optional<Browse> BrowseService::browseOf(const QString &peer) const
{
    const auto found = m_browses.constFind(peer);
    return found == m_browses.cend() ? std::nullopt : std::optional(found.value());
}

void BrowseService::listProgress(const QString &peer, qint64 received, qint64 total)
{
    const auto found = m_browses.find(peer);
    if (found == m_browses.end() || found->state == State::Ready) {
        return;
    }
    found->received = received;
    found->total = total;
    setState(*found, State::Receiving);
}

void BrowseService::listReceived(const QString &peer, const slsk::SharedFileList &list)
{
    if (!m_browses.contains(peer)) {
        return; // closed meanwhile (or never asked)
    }
    store(peer, list);
    accept(peer, list, m_now(), false);
}

void BrowseService::requestFailed(const QString &peer, const QString &why)
{
    const auto found = m_browses.find(peer);
    if (found == m_browses.end() || found->state == State::Ready) {
        return;
    }
    setState(*found, State::Failed, why);
}

void BrowseService::accept(const QString &peer, const slsk::SharedFileList &list, qint64 fetchedAtMs, bool fromCache)
{
    Browse &entry = m_browses[peer];
    entry.peer = peer;
    entry.fromCache = fromCache;
    entry.fetchedAtMs = fetchedAtMs;
    entry.folders = static_cast<int>(list.directories.size() + list.privateDirectories.size());
    entry.privateFolders = static_cast<int>(list.privateDirectories.size());
    entry.files = 0;
    for (const QList<slsk::SharedDirectory> *directories : {&list.directories, &list.privateDirectories}) {
        for (const slsk::SharedDirectory &directory : *directories) {
            entry.files += static_cast<int>(directory.files.size());
        }
    }
    setState(entry, State::Ready, entry.files == 0 ? tr("%1 shares nothing.").arg(peer) : QString());
    // Analysed like search results, on the worker: quality, what the user owns.
    const quint64 generation = ++m_generation[peer];
    m_pool.start([this, peer, generation, hits = hitsOf(peer, list), index = m_index] {
        Analysed analysed{.peer = peer, .generation = generation, .results = analyzeHits(hits, 0, index, std::nullopt)};
        {
            const std::scoped_lock lock(m_mutex);
            m_mailbox.append(std::move(analysed));
        }
        // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    });
}

void BrowseService::drain()
{
    QList<Analysed> analyses;
    {
        const std::scoped_lock lock(m_mutex);
        analyses.swap(m_mailbox);
    }
    for (Analysed &analysed : analyses) {
        if (analysed.generation != m_generation.value(analysed.peer) || !m_browses.contains(analysed.peer)) {
            continue;
        }
        if (m_annotate) {
            m_annotate(analysed.results);
        }
        m_results.insert(analysed.peer, analysed.results);
        Q_EMIT resultsReset(analysed.peer, analysed.results);
    }
}

void BrowseService::store(const QString &peer, const slsk::SharedFileList &list)
{
    const QByteArray blob = encodeList(list);
    if (blob.size() > kMaxCachedBytes) {
        return; // too big to keep: looked at once, asked again next time
    }
    Statement insert =
        m_db.prepare(u"INSERT OR REPLACE INTO browse_cache (peer, fetched_ms, list) VALUES (?1, ?2, ?3);");
    if (!insert.isValid() || !insert.bind(1, peer).bind(2, m_now()).bind(3, blob).run()) {
        qCWarning(lcBrowse) << "couldn't keep" << peer << "'s share list:" << m_db.lastError();
        return;
    }
    // A day at most, and the latest kCachedPeers.
    Statement prune = m_db.prepare(u"DELETE FROM browse_cache WHERE fetched_ms < ?1 OR peer NOT IN "
                                   "(SELECT peer FROM browse_cache ORDER BY fetched_ms DESC LIMIT ?2);");
    if (prune.isValid()) {
        prune.bind(1, m_now() - kCacheLifetimeMs).bind(2, kCachedPeers);
        if (!prune.run()) {
            qCWarning(lcBrowse) << "couldn't prune the browse cache:" << m_db.lastError();
        }
    }
}

std::optional<std::pair<slsk::SharedFileList, qint64>> BrowseService::cached(const QString &peer)
{
    Statement query = m_db.prepare(u"SELECT fetched_ms, list FROM browse_cache WHERE peer = ?1;");
    if (!query.isValid() || query.bind(1, peer).step() != Statement::Step::Row) {
        return std::nullopt;
    }
    const qint64 fetched = query.int64(0);
    if (m_now() - fetched > kCacheLifetimeMs) {
        return std::nullopt;
    }
    std::optional<slsk::SharedFileList> list = decodeList(query.blob(1));
    if (!list) {
        return std::nullopt;
    }
    return std::pair{std::move(*list), fetched};
}

int BrowseService::cachedPeers()
{
    Statement query = m_db.prepare(u"SELECT count(*) FROM browse_cache WHERE fetched_ms >= ?1;");
    if (query.isValid() && query.bind(1, m_now() - kCacheLifetimeMs).step() == Statement::Step::Row) {
        return static_cast<int>(query.int64(0));
    }
    return 0;
}

void BrowseService::clearCache()
{
    if (!m_db.exec(u"DELETE FROM browse_cache;"_s)) {
        qCWarning(lcBrowse) << "couldn't clear the browse cache:" << m_db.lastError();
    }
}

int BrowseService::download(const QString &peer, const QList<int> &indexes)
{
    const QList<SearchResult> results = m_results.value(peer);
    int accepted = 0;
    for (const int index : indexes) {
        if (index < 0 || index >= results.size() || !results.at(index).isAudio() || !m_enqueue) {
            continue;
        }
        ++accepted;
        const SearchResult &result = results.at(index);
        lookUpOwnRelease(this, m_resolve, result, [this, result](const std::optional<meta::Release> &release) {
            DownloadSpec spec;
            spec.peer = result.hit.peer;
            spec.remotePath = result.hit.path;
            spec.size = static_cast<qint64>(result.hit.size);
            spec.file = fileChoiceOf(result);
            spec.release = release;
            if (!m_enqueue || !m_enqueue(spec)) {
                qCWarning(lcBrowse) << "couldn't queue" << result.path << "from" << result.hit.peer;
            }
        });
    }
    return accepted;
}

bool BrowseService::downloadFolder(const QString &peer, int index)
{
    const QList<SearchResult> results = m_results.value(peer);
    if (index < 0 || index >= results.size() || !m_enqueueAlbum) {
        return false;
    }
    const SearchResult &anchor = results.at(index);
    const qsizetype separator = anchor.hit.path.lastIndexOf('\\');
    if (separator <= 0) {
        return false;
    }
    const QByteArray folder = anchor.hit.path.left(separator);
    const QByteArray prefix = anchor.hit.path.left(separator + 1);
    // The browse listed the whole folder: its files are the album (subfolders aren't part of it).
    QList<SearchHit> files;
    for (const SearchResult &result : results) {
        if (result.hit.path.startsWith(prefix) && !result.hit.path.mid(prefix.size()).contains('\\')) {
            files.append(result.hit);
        }
    }
    const QString owner = anchor.hit.peer;
    lookUpOwnRelease(
        this, m_resolve, anchor, [this, owner, folder, files](const std::optional<meta::Release> &release) {
            if (m_enqueueAlbum && !m_enqueueAlbum(owner, folder, files, release)) {
                qCWarning(lcBrowse) << "couldn't download the folder" << QString::fromUtf8(folder) << "from" << owner;
            }
        });
    return true;
}

void BrowseService::waitForIdle()
{
    m_pool.waitForDone();
    drain();
}

} // namespace selecta::core
