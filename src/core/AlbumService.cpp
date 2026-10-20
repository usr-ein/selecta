// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/AlbumService.h"

#include "core/ActivityLog.h"
#include "core/CollectionStore.h"
#include "core/MatchCorrections.h"
#include "core/MetaServices.h"
#include "core/NetworkService.h"
#include "core/TransferService.h"
#include "governor/Governor.h"
#include "library/HistoryStore.h"
#include "match/Quality.h"
#include "match/RemotePathParser.h"
#include "match/Supersede.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcAlbums, "selecta.core.albums")

using State = AlbumDownload::State;

// Collection ids, apart from Inbox ids in one list of singles.
constexpr quint64 kCollectionIdBit = quint64{1} << 62U;

bool isImageName(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == u"jpg"_s || suffix == u"jpeg"_s || suffix == u"png"_s;
}

bool isAudioName(const QString &name)
{
    return match::RemotePathParser::fileKind(QFileInfo(name).suffix().toLower()) == match::FileKind::Audio;
}

// "FLAC 16/44.1", "MP3 320", "MP3 VBR 245" (as recorded on arrival) back into the facts supersede compares.
match::AudioQuality qualityFromText(const QString &text)
{
    match::AudioQuality quality;
    const QStringList words = text.split(u' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return quality;
    }
    const QString codec = words.constFirst().toLower();
    quality.codec = codec == u"vorbis"_s ? match::Codec::Vorbis : match::codecFromExtension(codec);
    const QString &last = words.constLast();
    if (last.contains(u'/')) {
        quality.bitDepth = last.section(u'/', 0, 0).toInt();
        quality.sampleRateHz = static_cast<int>(last.section(u'/', 1, 1).toDouble() * 1000.0);
    } else if (words.size() > 1) {
        quality.bitrateKbps = last.toInt();
        quality.vbr = words.contains(u"VBR"_s);
    }
    return quality;
}

QList<slsk::FileEntry> entriesOf(const QList<SearchHit> &hits)
{
    QList<slsk::FileEntry> files;
    files.reserve(hits.size());
    for (const SearchHit &hit : hits) {
        files.append(slsk::FileEntry{.name = slsk::RemotePath{hit.path},
                                     .size = hit.size,
                                     .extension = hit.extension,
                                     .attributes = hit.attributes});
    }
    return files;
}

QString displayName(const QString &artist, const QString &title)
{
    return artist.isEmpty() ? title : artist + u" – "_s + title;
}

} // namespace

AlbumService::AlbumService(storage::Database &db, governor::Governor &governor, NetworkService &network,
                           TransferService &transfers, InboxService &inbox, ActivityLog &activity,
                           library::FileOps::TrashFunction trash, Options options, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_governor(governor)
    , m_network(network)
    , m_transfers(transfers)
    , m_inbox(inbox)
    , m_activity(activity)
    , m_trash(trash ? std::move(trash) : library::FileOps::TrashFunction(&library::moveToSystemTrash))
    , m_options(std::move(options))
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_store(db)
    , m_downloads(db)
{
    qRegisterMetaType<AlbumDownload>();
    qRegisterMetaType<AlbumReport>();
    m_pool.setMaxThreadCount(1); // one album at a time: each one already converts a dozen files
    m_pool.setObjectName(u"selecta-albums"_s);
    connect(&m_network.peers(), &slsk::PeerNetwork::folderContentsReceived, this,
            &AlbumService::folderContentsReceived);
}

AlbumService::~AlbumService()
{
    for (const std::shared_ptr<audio::CancellationFlag> &cancel : std::as_const(m_arriving)) {
        cancel->cancel(); // the album is made again at the next launch: its files stay in Converting
    }
    m_pool.waitForDone();
}

void AlbumService::setState(AlbumDownload &album, AlbumDownload::State state, const QString &message)
{
    album.state = state;
    album.message = message;
    album.updatedAt = m_now();
    if (!m_store.update(album)) {
        qCWarning(lcAlbums) << "couldn't save album download" << album.id << ':' << m_db.lastError();
    }
    Q_EMIT albumChanged(album);
}

std::optional<qint64> AlbumService::download(const QString &peer, const QByteArray &folder,
                                             const QList<SearchHit> &known, const std::optional<meta::Release> &release,
                                             bool complete)
{
    if (peer.isEmpty() || folder.isEmpty() || m_inbox.config().inboxRoot.isEmpty()) {
        return std::nullopt;
    }
    AlbumDownload album;
    album.peer = peer;
    album.folder = folder;
    album.release = release;
    album.state = State::Listing;
    album.message = tr("Asking %1 what the folder holds").arg(peer);
    album.createdAt = m_now();
    album.updatedAt = album.createdAt;
    const std::optional<qint64> id = m_store.add(album);
    if (!id) {
        qCWarning(lcAlbums) << "couldn't save an album download:" << m_db.lastError();
        return std::nullopt;
    }
    album.id = *id;
    m_activity.record(u"album"_s, peer, u"wanted"_s, album.folderName());
    Q_EMIT albumChanged(album);
    if (complete) {
        listed(album.id, entriesOf(known), true); // a browse listed the whole folder already
    } else {
        requestListing(album, known);
    }
    return album.id;
}

void AlbumService::requestListing(const AlbumDownload &album, const QList<SearchHit> &known)
{
    // The whole folder, as the peer lists it now: the search only showed the files that matched.
    governor::Decision decision = m_governor.request(governor::Operation::FolderContents, {.peer = album.peer});
    quint32 token = 0;
    if (decision.permit && m_network.isOnline()) {
        token = m_network.peers().requestFolderContents(album.peer, slsk::RemotePath{album.folder},
                                                        std::move(*decision.permit));
    }
    if (token == 0) {
        listed(album.id, entriesOf(known), false);
        return;
    }
    const auto listing = std::make_shared<Listing>();
    listing->albumId = album.id;
    listing->known = known;
    listing->timeout = std::make_unique<QTimer>();
    listing->timeout->setSingleShot(true);
    listing->timeout->setInterval(m_options.listingTimeout);
    connect(listing->timeout.get(), &QTimer::timeout, this, [this, token] {
        if (const std::shared_ptr<Listing> waited = m_listings.take(token)) {
            listed(waited->albumId, entriesOf(waited->known), false);
        }
    });
    listing->timeout->start();
    m_listings.insert(token, listing);
}

void AlbumService::folderContentsReceived(const QString &username, const slsk::peer::FolderContentsResponse &contents)
{
    Q_UNUSED(username)
    const std::shared_ptr<Listing> listing = m_listings.take(contents.token);
    if (!listing) {
        return;
    }
    // Every file of the folder and of its own folders (an album's "CD1", "CD2"), with full paths.
    QList<slsk::FileEntry> files;
    for (const slsk::SharedDirectory &directory : contents.directories) {
        for (const slsk::FileEntry &file : directory.files) {
            slsk::FileEntry full = file;
            full.name = slsk::RemotePath{directory.path.raw + '\\' + file.name.raw};
            files.append(full);
        }
    }
    // An empty listing: the search's files are all there is to go on.
    listed(listing->albumId, files.isEmpty() ? entriesOf(listing->known) : files, !files.isEmpty());
}

void AlbumService::listed(qint64 albumId, const QList<slsk::FileEntry> &files, bool fromPeer)
{
    std::optional<AlbumDownload> album = m_store.find(albumId);
    if (!album || album->isFinished()) {
        return;
    }
    // The album's audio and its images; cue sheets, logs and the like stay behind.
    int queued = 0;
    for (const slsk::FileEntry &file : files) {
        const QString name = file.name.display();
        if (!isAudioName(name) && !isImageName(name)) {
            continue;
        }
        DownloadSpec spec;
        spec.peer = album->peer;
        spec.remotePath = file.name.raw;
        spec.size = static_cast<qint64>(file.size);
        spec.album = album->id;
        if (m_transfers.enqueue(spec)) {
            ++queued;
        }
    }
    if (queued == 0) {
        setState(*album, State::Failed,
                 fromPeer ? tr("The folder holds no audio file.") : tr("Nothing to download from this folder."));
        m_activity.record(u"album"_s, album->peer, u"failed"_s, album->message);
        return;
    }
    setState(*album, State::Downloading,
             queued == 1 ? tr("Downloading 1 file") : tr("Downloading %1 files").arg(queued));
    checkComplete(album->id);
}

bool AlbumService::fileCompleted(const Download &download)
{
    if (!download.album) {
        return false;
    }
    const std::optional<AlbumDownload> album = m_store.find(*download.album);
    if (!album || album->isFinished()) {
        return false;
    }
    checkComplete(album->id);
    return true;
}

void AlbumService::downloadChanged(const Download &download)
{
    if (download.album && (download.state == Download::State::Failed || download.state == Download::State::Cancelled)) {
        checkComplete(*download.album);
    }
}

void AlbumService::checkComplete(qint64 albumId)
{
    if (m_arriving.contains(albumId)) {
        return;
    }
    std::optional<AlbumDownload> album = m_store.find(albumId);
    if (!album || album->isFinished() || album->state == State::Listing) {
        return;
    }
    const QList<Download> downloads = m_downloads.forAlbum(albumId);
    if (downloads.isEmpty()) {
        return;
    }
    for (const Download &download : downloads) {
        const bool done = download.state == Download::State::Failed || download.state == Download::State::Cancelled ||
                          download.state == Download::State::Arrived || download.state == Download::State::Converting;
        if (!done) {
            return; // still coming
        }
    }
    startArrival(*album);
}

void AlbumService::restore()
{
    const QList<AlbumDownload> albums = m_store.unfinished();
    for (const AlbumDownload &album : albums) {
        Q_EMIT albumChanged(album);
        if (album.state == State::Listing && m_downloads.forAlbum(album.id).isEmpty()) {
            requestListing(album, {}); // the answer was lost with the quit: ask again
            continue;
        }
        if (album.state == State::Listing) {
            AlbumDownload listed = album;
            setState(listed, State::Downloading, tr("Downloading"));
        }
        checkComplete(album.id);
    }
}

void AlbumService::cancel(qint64 albumId)
{
    std::optional<AlbumDownload> album = m_store.find(albumId);
    if (!album || album->isFinished()) {
        return;
    }
    if (const auto arriving = m_arriving.constFind(albumId); arriving != m_arriving.cend()) {
        arriving.value()->cancel();
    }
    setState(*album, State::Cancelled, tr("Cancelled"));
    const QList<Download> downloads = m_downloads.forAlbum(albumId);
    for (const Download &download : downloads) {
        if (!download.isFinished()) {
            m_transfers.cancel(download.id);
        }
    }
    m_activity.record(u"album"_s, album->peer, u"cancelled"_s, album->folderName());
}

QList<AlbumDownload> AlbumService::unfinished()
{
    return m_store.unfinished();
}

void AlbumService::startArrival(AlbumDownload album)
{
    m_arriving.insert(album.id, std::make_shared<audio::CancellationFlag>());
    setState(album, State::Converting, tr("Making the album"));
    const auto pending = std::make_shared<PendingArrival>();
    pending->job = std::make_shared<AlbumArrivalJob>();
    AlbumArrivalJob &job = *pending->job;
    job.albumId = album.id;
    job.release = album.release;
    job.folderName = album.folderName();
    job.peer = album.peer;
    job.library = m_inbox.config();
    job.stagingDir = m_options.stagingDir;
    job.encodedBy = m_options.encodedBy;
    job.trash = m_trash;
    const QList<Download> files = m_downloads.forAlbum(album.id);
    for (const Download &download : files) {
        pending->downloads.append(download.id);
        if (download.state == Download::State::Converting && QFileInfo::exists(download.localPath)) {
            job.files.append(AlbumArrivalFile{
                .downloadId = download.id, .localPath = download.localPath, .remoteName = download.remoteFileName()});
        } else if (isAudioName(download.remoteFileName())) {
            ++job.missing; // a missing image doesn't make the album incomplete
        }
    }

    // The album's track list (Spotify, from the release picked), then its cover, then the worker. The lookups are
    // bounded: past the timeout the album is made with what is known by then.
    QTimer::singleShot(m_options.metadataTimeout, this, [this, weak = std::weak_ptr<PendingArrival>(pending)] {
        if (const std::shared_ptr<PendingArrival> pending = weak.lock()) {
            runArrival(pending);
        }
    });
    const QPointer<AlbumService> self(this);
    const auto withCover = [self, pending] {
        if (!self || pending->started) {
            return;
        }
        meta::CoverFetcher *covers = self->m_meta ? self->m_meta->covers() : nullptr;
        if (!covers || !pending->job->release) {
            self->runArrival(pending);
            return;
        }
        covers->fetch(*pending->job->release, meta::Priority::Background)
            .then(self.data(), [self, pending](const meta::Result<meta::CoverImage> &cover) {
                if (!self || pending->started) {
                    return;
                }
                if (cover.ok()) {
                    pending->job->cover = cover.value().bytes;
                } else {
                    qCInfo(lcAlbums).noquote() << "no album cover:" << cover.error().message;
                }
                self->runArrival(pending);
            });
    };
    const bool spotify =
        m_meta && job.release && !job.release->spotifyAlbumId.isEmpty() && m_meta->spotify().isAvailable();
    if (!spotify) {
        withCover();
        return;
    }
    m_meta->spotify()
        .album(job.release->spotifyAlbumId, meta::Priority::Background)
        .then(this, [pending, withCover](const meta::Result<meta::Album> &found) {
            if (found.ok()) {
                pending->job->tracks = found.value().tracks;
            } else {
                qCInfo(lcAlbums).noquote() << "no track list from Spotify:" << found.error().message;
            }
            withCover();
        });
}

void AlbumService::runArrival(const std::shared_ptr<PendingArrival> &pending)
{
    const std::shared_ptr<audio::CancellationFlag> cancel = m_arriving.value(pending->job->albumId);
    if (pending->started || !cancel) {
        return;
    }
    pending->started = true;
    m_pool.start([this, job = pending->job, downloads = pending->downloads, cancel] {
        Outcome outcome{.albumId = job->albumId, .downloads = downloads, .result = arriveAlbum(*job, cancel.get())};
        {
            const std::scoped_lock lock(m_mutex);
            m_outcomes.append(std::move(outcome));
        }
        // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    });
}

void AlbumService::drain()
{
    QList<Outcome> outcomes;
    {
        const std::scoped_lock lock(m_mutex);
        outcomes.swap(m_outcomes);
    }
    for (const Outcome &outcome : std::as_const(outcomes)) {
        recordArrival(outcome);
    }
}

void AlbumService::recordArrival(const Outcome &outcome)
{
    m_arriving.remove(outcome.albumId);
    std::optional<AlbumDownload> album = m_store.find(outcome.albumId);
    if (!album || album->state == State::Cancelled) {
        return;
    }
    const AlbumArrivalResult &result = outcome.result;
    if (!result.ok) {
        if (result.cancelled) {
            return; // made again at the next launch
        }
        for (const qint64 id : outcome.downloads) {
            m_transfers.arrivalFailed(id, result.error);
        }
        setState(*album, State::Failed, result.error);
        m_activity.record(u"album"_s, album->peer, u"failed"_s, result.error);
        return;
    }
    library::InboxItem item = result.item;
    item.session = m_options.session;
    item.createdAt = m_now();
    const std::optional<qint64> itemId = m_inbox.add(item);
    if (!itemId) {
        const QString error =
            tr("The album is in your Inbox folder, but Selecta couldn't record it: %1").arg(m_db.lastError());
        for (const qint64 id : outcome.downloads) {
            m_transfers.arrivalFailed(id, error);
        }
        setState(*album, State::Failed, error);
        return;
    }
    // Each track into the history (DL-9), each download done.
    library::HistoryStore history(m_db);
    QSet<qint64> tracked;
    for (const AlbumArrivalResult::Track &track : result.tracks) {
        tracked.insert(track.downloadId);
        if (!track.ok) {
            m_transfers.arrivalFailed(track.downloadId, track.error);
            continue;
        }
        if (const std::optional<Download> download = m_downloads.find(track.downloadId)) {
            library::HistoryEntry entry;
            entry.session = m_options.session;
            entry.completedAt = item.createdAt;
            entry.kind = library::HistoryEntry::Kind::Album;
            entry.peer = download->peer;
            entry.remotePath = download->remotePath;
            entry.sizeBytes = download->size > 0 ? download->size : download->received;
            entry.quality = track.sourceQuality;
            entry.artist = track.artist;
            entry.title = track.title;
            entry.inboxItem = itemId;
            entry.whereabouts = library::HistoryEntry::Whereabouts::Inbox;
            if (!history.add(entry)) {
                qCWarning(lcAlbums) << "couldn't record" << track.title << "in the history:" << m_db.lastError();
            }
        }
        m_transfers.arrived(track.downloadId, *itemId);
    }
    for (const qint64 id : outcome.downloads) {
        if (!tracked.contains(id)) {
            m_transfers.arrived(id, *itemId); // the images (files that never came are already failed)
        }
    }
    album->inboxItem = itemId;
    setState(*album, result.complete ? State::Arrived : State::Partial,
             result.warning.isEmpty() ? tr("In your Inbox as %1").arg(item.relativePath) : result.warning);
    m_activity.record(u"album"_s, album->peer, u"arrived"_s,
                      result.warning.isEmpty() ? item.relativePath
                                               : item.relativePath + u" ("_s + result.warning + u')');
    AlbumReport report;
    report.albumId = album->id;
    report.album = item.album.isEmpty() ? item.title : item.album;
    report.arrival = true;
    report.complete = result.complete;
    report.tracks = item.trackCount.value_or(0);
    report.where = item.relativePath;
    report.warning = result.warning;
    supersede(*album, result, report);
    Q_EMIT reported(report);
}

void AlbumService::supersede(const AlbumDownload &album, const AlbumArrivalResult &result, AlbumReport &report)
{
    QList<match::SupersedeAlbumTrack> tracks;
    for (const AlbumArrivalResult::Track &track : result.tracks) {
        if (track.ok) {
            tracks.append({.id = static_cast<quint64>(track.downloadId),
                           .track = {.artist = track.artist,
                                     .title = track.title,
                                     .durationMs = track.durationMs,
                                     .quality = qualityFromText(track.sourceQuality)}});
        }
    }
    if (tracks.isEmpty()) {
        return;
    }
    // Singles still in the Inbox, compared on the quality they were downloaded in (the file itself is MP3 320).
    QList<match::SupersedeSingle> singles;
    QHash<quint64, QString> names;
    int inboxSingles = 0;
    const QList<InboxEntry> entries = m_inbox.entries();
    for (const InboxEntry &entry : entries) {
        if (entry.item.kind != library::InboxItem::Kind::Single) {
            continue;
        }
        ++inboxSingles;
        QString title = entry.item.title;
        if (!entry.item.version.isEmpty() && !title.contains(entry.item.version, Qt::CaseInsensitive)) {
            title += u" ("_s + entry.item.version + u')';
        }
        const auto id = static_cast<quint64>(entry.item.id);
        names.insert(id, displayName(entry.item.artist, title));
        singles.append({.id = id,
                        .track = {.artist = entry.item.artist,
                                  .title = title,
                                  .durationMs = entry.item.durationMs,
                                  .quality = qualityFromText(entry.item.sourceFormat)},
                        .place = match::SinglePlace::Inbox,
                        .busy = entry.busy});
    }
    // Singles filed in the collection are only ever flagged (DL-8, D3): rekordbox links to them.
    QHash<quint64, QString> collectionPaths;
    const collection::FolderRoles folders = m_inbox.config().folderRoles();
    if (!folders.tracks().isEmpty()) {
        const QList<CollectionFile> files = CollectionStore(m_db).all();
        for (const CollectionFile &file : files) {
            if (folders.roleOf(file.file.file.path) != collection::FolderRoles::Role::Track) {
                continue; // the collection's singles are the loose tracks of the tracks folder
            }
            const quint64 id = static_cast<quint64>(file.id) | kCollectionIdBit;
            names.insert(id, displayName(file.file.artist, file.file.title));
            collectionPaths.insert(id, file.file.file.path);
            singles.append({.id = id,
                            .track = {.artist = file.file.artist,
                                      .title = file.file.title,
                                      .durationMs = file.file.durationMs,
                                      .quality = file.file.quality},
                            .place = match::SinglePlace::Collection});
        }
    }
    const match::AlbumDownloadState state =
        result.complete ? match::AlbumDownloadState::CompleteVerified : match::AlbumDownloadState::Partial;
    const match::SupersedeDecision decision = match::SupersedeDecider::decide(state, tracks, singles, inboxSingles);
    // LIB-6: both sides as corrections know them; a pair the user said isn't the same track is no match.
    QHash<quint64, TrackIdentity> albumTracks;
    for (const match::SupersedeAlbumTrack &track : std::as_const(tracks)) {
        albumTracks.insert(track.id, trackIdentity(track.track.artist, track.track.title, track.track.durationMs));
    }
    QHash<quint64, TrackIdentity> singleTracks;
    for (const match::SupersedeSingle &single : std::as_const(singles)) {
        singleTracks.insert(single.id, trackIdentity(single.track.artist, single.track.title, single.track.durationMs));
    }
    MatchCorrectionStore corrections(m_db);
    const bool corrected = !corrections.isEmpty();
    Held held{.album = report.album, .peer = album.peer, .singles = {}, .names = {}};
    QList<qint64> trash;
    SupersedeFlagStore flags(m_db);
    bool flagged = false;
    // DL-8: the pair stays visible in the Inbox and Collection views until the single goes.
    const auto flag = [&](const match::SupersedePair &pair) {
        SupersedeFlag entry;
        entry.album = report.album;
        entry.albumItem = album.inboxItem;
        if (const auto path = collectionPaths.constFind(pair.singleId); path != collectionPaths.cend()) {
            entry.singlePath = path.value();
        } else {
            entry.singleItem = static_cast<qint64>(pair.singleId);
        }
        entry.single = names.value(pair.singleId);
        entry.summary = pair.summary;
        entry.createdAt = m_now();
        entry.singleTrack = singleTracks.value(pair.singleId);
        entry.albumTrack = albumTracks.value(pair.albumTrackId);
        if (flags.add(entry)) {
            flagged = true;
        } else {
            qCWarning(lcAlbums) << "couldn't flag" << entry.single << ':' << m_db.lastError();
        }
    };
    for (const match::SupersedePair &pair : decision.pairs) {
        if (corrected && corrections.verdict(CorrectionKind::Supersede, singleTracks.value(pair.singleId),
                                             albumTracks.value(pair.albumTrackId)) == Verdict::Different) {
            m_activity.record(
                u"supersede"_s, album.peer, u"corrected"_s,
                tr("%1: not the same track as on “%2”, as you said").arg(names.value(pair.singleId), report.album));
            continue; // neither flagged nor trashed
        }
        m_activity.record(u"supersede"_s, album.peer, u"decided"_s, pair.summary);
        const auto single = static_cast<qint64>(pair.singleId);
        if (pair.action == match::SupersedeAction::Trash && decision.needsConfirmation) {
            held.singles.append(single); // the circuit breaker: so many at once that the user says first (T42)
            held.names.append(names.value(pair.singleId));
            flag(pair);
        } else if (pair.action == match::SupersedeAction::Trash) {
            trash.append(single);
            m_supersedeTrash.insert(single, album.id);
            report.trashed.append(names.value(pair.singleId));
        } else if (pair.action == match::SupersedeAction::Flag) {
            report.flagged.append(names.value(pair.singleId));
            flag(pair);
        }
    }
    if (!trash.isEmpty()) {
        m_inbox.trash(trash);
    }
    if (!held.singles.isEmpty()) {
        m_activity.record(u"supersede"_s, album.peer, u"asked"_s, decision.confirmationReason);
        report.held = held.names;
        m_held.insert(album.id, std::move(held));
    }
    if (flagged) {
        m_inbox.refresh(); // the Inbox shows "also on" with its singles
        Q_EMIT flagsChanged();
    }
}

void AlbumService::confirmSupersede(qint64 albumId)
{
    const Held held = m_held.take(albumId);
    AlbumReport report;
    report.albumId = albumId;
    report.album = held.album;
    QList<qint64> trash;
    for (qsizetype i = 0; i < held.singles.size(); ++i) {
        const qint64 single = held.singles.at(i);
        if (m_inbox.item(single)) { // still in the Inbox
            trash.append(single);
            m_supersedeTrash.insert(single, albumId);
            report.trashed.append(held.names.value(i));
        }
    }
    if (trash.isEmpty()) {
        return;
    }
    m_activity.record(u"supersede"_s, held.peer, u"confirmed"_s,
                      trash.size() == 1 ? tr("1 single to the Trash")
                                        : tr("%1 singles to the Trash").arg(trash.size()));
    m_inbox.trash(trash);
    Q_EMIT reported(report);
}

void AlbumService::inboxItemTrashed(const TrashedItem &trashed)
{
    const auto found = m_supersedeTrash.constFind(trashed.item.id);
    if (found == m_supersedeTrash.cend()) {
        return;
    }
    m_undo[found.value()].append(trashed);
    m_supersedeTrash.erase(found);
}

void AlbumService::undoSupersede(qint64 albumId)
{
    const QList<TrashedItem> trashed = m_undo.take(albumId);
    for (const TrashedItem &single : trashed) {
        m_inbox.restore(single);
    }
    if (!trashed.isEmpty()) {
        if (const std::optional<AlbumDownload> album = m_store.find(albumId)) {
            m_activity.record(u"supersede"_s, album->peer, u"undone"_s,
                              trashed.size() == 1 ? tr("1 single put back in the Inbox")
                                                  : tr("%1 singles put back in the Inbox").arg(trashed.size()));
        }
    }
}

std::optional<AlbumDownload> AlbumService::album(qint64 id)
{
    return m_store.find(id);
}

void AlbumService::waitForIdle()
{
    for (int round = 0; round < 100; ++round) {
        m_pool.waitForDone();
        bool pending = false;
        {
            const std::scoped_lock lock(m_mutex);
            pending = !m_outcomes.isEmpty();
        }
        if (!pending) {
            return;
        }
        drain();
    }
}

} // namespace selecta::core
