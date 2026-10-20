// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CollectionService.h"

#include "collection/Albums.h"
#include "collection/Watcher.h"
#include "core/CollectionStore.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcCollection, "selecta.core.collection")

// "MP3 320", "MP3 below 320", "FLAC"…: how the Library view counts formats (LIB-5).
QString formatLabel(const match::AudioQuality &quality)
{
    switch (quality.codec) {
    case match::Codec::Mp3:
        return quality.bitrateKbps >= 315 ? u"MP3 320"_s : u"MP3 below 320"_s;
    case match::Codec::Flac:
        return u"FLAC"_s;
    case match::Codec::Aiff:
        return u"AIFF"_s;
    case match::Codec::Wav:
        return u"WAV"_s;
    case match::Codec::Aac:
        return u"AAC"_s;
    case match::Codec::Alac:
        return u"ALAC"_s;
    default:
        return u"Other"_s;
    }
}

match::Codec codecOf(const QString &format)
{
    const QString name = format.toLower();
    if (name == u"mp3") {
        return match::Codec::Mp3;
    }
    if (name == u"flac") {
        return match::Codec::Flac;
    }
    if (name == u"aiff" || name == u"aif") {
        return match::Codec::Aiff;
    }
    if (name == u"wav") {
        return match::Codec::Wav;
    }
    if (name == u"aac" || name == u"m4a") {
        return match::Codec::Aac;
    }
    if (name == u"alac") {
        return match::Codec::Alac;
    }
    return match::Codec::Unknown;
}

// Whether `path` is one of `folders` or inside one of them.
bool coveredBy(const QString &path, const QStringList &folders)
{
    const QString key = collection::comparisonKey(path);
    return std::ranges::any_of(folders, [&key](const QString &folder) {
        const QString folderKey = collection::comparisonKey(folder);
        return key == folderKey || key.startsWith(folderKey.endsWith(u'/') ? folderKey : folderKey + u'/');
    });
}

} // namespace

CollectionService::CollectionService(storage::Database &db, Options options, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_options(options)
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
    , m_rebuildTimer(new QTimer(this))
{
    qRegisterMetaType<CollectionStatus>();
    qRegisterMetaType<std::shared_ptr<const match::MatchIndex>>();
    m_pool.setMaxThreadCount(1); // scans and builds take turns; a scan has reader threads of its own
    m_pool.setObjectName(u"selecta-collection"_s);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(m_options.rebuildDelayMs);
    connect(m_rebuildTimer, &QTimer::timeout, this, &CollectionService::rebuild);
    if (m_options.watch) {
        m_watcher = new collection::Watcher(this);
        connect(m_watcher, &collection::Watcher::changed, this, [this](const QStringList &folders) {
            if (m_scanning) {
                for (const QString &folder : folders) {
                    if (!m_foldersWanted.contains(folder)) {
                        m_foldersWanted.append(folder);
                    }
                }
            } else {
                startScan(folders, false);
            }
        });
    }
    m_status.tracks = CollectionStore(m_db).count();
}

CollectionService::~CollectionService()
{
    if (m_cancel) {
        m_cancel->cancel();
    }
    if (m_watcher) {
        m_watcher->stop();
    }
    m_pool.waitForDone(); // results still in the mailbox are dropped with it
}

void CollectionService::setFolders(const collection::FolderRoles &folders, const QStringList &excluded)
{
    const bool changed = !m_rootsSet || folders != m_folders || excluded != m_excluded;
    m_folders = folders;
    m_roots = folders.roots();
    m_excluded = excluded;
    m_rootsSet = true;
    m_status.roots = m_roots;
    if (!changed) {
        return;
    }
    CollectionStore store(m_db);
    const int forgotten = store.removeOutside(m_roots);
    if (forgotten > 0) {
        qCInfo(lcCollection) << "forgot" << forgotten << "files outside the collection folders";
    }
    m_status.tracks = store.count();
    if (m_watcher) {
        m_watcher->stop();
        if (!m_roots.isEmpty() && !m_watcher->watch(m_roots)) {
            qCInfo(lcCollection).noquote() << "not watching the collection:" << m_watcher->errorString();
        }
    }
    if (m_roots.isEmpty()) {
        m_status.message.clear();
        publish();
        scheduleRebuild();
        return;
    }
    scanNow();
    scheduleRebuild(); // the albums follow the roles, even when the scan finds no file to add or remove
}

void CollectionService::scanNow()
{
    if (m_roots.isEmpty()) {
        return;
    }
    if (m_scanning) {
        m_fullScanWanted = true;
        if (m_cancel) {
            m_cancel->cancel(); // the full scan that follows covers it
        }
        return;
    }
    startScan(m_roots, true);
}

void CollectionService::setInbox(const QList<library::InboxItem> &items)
{
    m_inboxItems = items;
    scheduleRebuild();
}

void CollectionService::setHistory(const QList<library::HistoryEntry> &entries)
{
    m_history = entries;
    scheduleRebuild();
}

void CollectionService::startScan(const QStringList &folders, bool full)
{
    CollectionStore store(m_db);
    QHash<QString, collection::FileSignature> known;
    QStringList scanned;
    if (full) {
        known = store.signatures();
        scanned = m_roots;
    } else {
        for (const QString &folder : folders) {
            // Outside the collection, or where the roles leave everything out (a subfolder of the tracks folder):
            // nothing to rescan. A collection folder inside the changed one is rescanned with its own role.
            const QStringList targets = m_folders.rescanFor(folder);
            for (const QString &target : targets) {
                if (coveredBy(target, m_excluded) || scanned.contains(target)) {
                    continue; // Selecta's own folders are never part of the collection
                }
                scanned.append(target);
                known.insert(store.signaturesUnder(target));
            }
        }
        if (scanned.isEmpty()) {
            return;
        }
    }
    m_scanning = true;
    m_status.scanning = true;
    m_status.done = 0;
    m_status.total = 0;
    publish();
    const auto cancel = std::make_shared<audio::CancellationFlag>();
    m_cancel = cancel;
    collection::ScanOptions options;
    options.readers = m_options.readers;
    options.excludedFolders = m_excluded;
    m_folders.applyTo(options);
    m_pool.start([this, scanned, known = std::move(known), options, cancel, full] {
        QElapsedTimer sinceReport;
        sinceReport.start();
        collection::ScanResult result =
            collection::scan(scanned, known, options, cancel.get(), [this, &sinceReport](int done, int total) {
                // Through the mailbox like the result, about four times a second.
                if (sinceReport.elapsed() >= 250 || done == total) {
                    sinceReport.restart();
                    post(ProgressOutcome{done, total});
                }
            });
        post(ScanOutcome{std::move(result), scanned, full});
    });
}

void CollectionService::scanFinished(const ScanOutcome &outcome)
{
    m_scanning = false;
    m_cancel.reset();
    const collection::ScanResult &result = outcome.result;
    CollectionStore store(m_db);
    // A cancelled scan still records what it read; it removes nothing (scan() guarantees that).
    if (!store.apply(result.added, result.changed, result.removed)) {
        qCWarning(lcCollection) << "couldn't record the scan:" << m_db.lastError();
    }
    m_status.scanning = false;
    m_status.tracks = store.count();
    if (outcome.full && !result.cancelled) {
        m_status.unreadable = static_cast<int>(result.unreadable.size());
        m_status.unreadableSample = result.unreadable.mid(0, kUnreadableSample);
        m_status.lastScanMs = m_now();
    }
    QStringList missing;
    for (const QString &root : std::as_const(m_roots)) {
        if (!QFileInfo(root).isDir()) {
            missing.append(QDir::toNativeSeparators(root));
        }
    }
    m_status.message = missing.isEmpty()
                           ? QString()
                           : tr("Not there: %1. Its tracks are kept until it is back.").arg(missing.join(u", "_s));
    qCInfo(lcCollection).noquote() << u"scan: %1 new, %2 changed, %3 gone, %4 unreadable%5"_s.arg(result.added.size())
                                          .arg(result.changed.size())
                                          .arg(result.removed.size())
                                          .arg(result.unreadable.size())
                                          .arg(result.cancelled ? u" (cancelled)"_s : QString());
    publish();
    if (!result.added.isEmpty() || !result.changed.isEmpty() || !result.removed.isEmpty() || !m_index) {
        scheduleRebuild();
    }
    if (m_fullScanWanted) {
        m_fullScanWanted = false;
        m_foldersWanted.clear();
        startScan(m_roots, true);
    } else if (!m_foldersWanted.isEmpty()) {
        const QStringList folders = std::exchange(m_foldersWanted, {});
        startScan(folders, false);
    }
}

void CollectionService::scheduleRebuild()
{
    m_rebuildTimer->start();
}

void CollectionService::rebuild()
{
    if (m_building) {
        m_rebuildWanted = true;
        return;
    }
    m_building = true;
    // Gathered here, where the database lives; built on the worker.
    QList<collection::ScannedFile> files;
    QList<qint64> ids;
    const QList<CollectionFile> stored = CollectionStore(m_db).all();
    files.reserve(stored.size());
    ids.reserve(stored.size());
    QHash<QString, int> formats;
    qint64 bytes = 0;
    for (const CollectionFile &file : stored) {
        files.append(file.file);
        ids.append(file.id);
        ++formats[formatLabel(file.file.quality)];
        bytes += file.file.file.size;
    }
    // LIB-5: what the collection is made of.
    m_status.bytes = bytes;
    m_status.formats.clear();
    for (auto it = formats.cbegin(); it != formats.cend(); ++it) {
        m_status.formats.append({it.key(), it.value()});
    }
    std::ranges::sort(m_status.formats, [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QList<match::LibraryTrack> inbox;
    for (const library::InboxItem &item : std::as_const(m_inboxItems)) {
        match::LibraryTrack track;
        track.id = static_cast<quint64>(item.id);
        track.source = match::Source::Inbox;
        track.artist = item.artist;
        track.title = item.version.isEmpty() || item.title.contains(item.version, Qt::CaseInsensitive)
                          ? item.title
                          : u"%1 (%2)"_s.arg(item.title, item.version);
        track.album = item.album;
        track.durationMs = item.durationMs;
        track.quality.codec = codecOf(item.format);
        track.quality.bitrateKbps = item.bitrate.value_or(0);
        track.quality.sampleRateHz = item.sampleRate.value_or(0);
        track.quality.bitDepth = item.bitDepth.value_or(0);
        inbox.append(track);
    }
    QList<match::HistoryEntry> history;
    for (const library::HistoryEntry &entry : std::as_const(m_history)) {
        history.append({static_cast<quint64>(entry.id), entry.peer, QString::fromUtf8(entry.remotePath),
                        static_cast<quint64>(std::max<qint64>(0, entry.sizeBytes))});
    }
    const quint64 generation = ++m_generation;
    m_pool.start([this, files = std::move(files), ids = std::move(ids), inbox = std::move(inbox),
                  history = std::move(history), folders = m_folders, generation] {
        QHash<QString, quint64> albumOf;
        const QList<match::LibraryAlbum> albums = collection::albumsOf(files, folders, &albumOf);
        match::MatchIndexInput input;
        input.tracks.reserve(files.size() + inbox.size());
        for (qsizetype i = 0; i < files.size(); ++i) {
            const collection::ScannedFile &file = files.at(i);
            match::LibraryTrack track;
            track.id = static_cast<quint64>(ids.at(i));
            track.source = match::Source::Collection;
            track.artist = file.artist;
            track.title = file.title;
            track.album = file.album;
            track.albumArtist = file.albumArtist;
            track.durationMs = file.durationMs;
            track.quality = file.quality;
            track.albumId = albumOf.value(file.file.path);
            track.trackNumber = file.trackNumber;
            track.discNumber = file.discNumber;
            input.tracks.append(track);
        }
        input.tracks.append(inbox);
        input.albums = albums;
        input.history = history;
        input.generation = generation;
        post(IndexOutcome{match::MatchIndex::build(input), static_cast<int>(albums.size())});
    });
}

void CollectionService::post(Outcome outcome)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_outcomes.append(std::move(outcome));
    }
    // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
    QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
}

void CollectionService::drain()
{
    QList<Outcome> outcomes;
    {
        const std::scoped_lock lock(m_mutex);
        outcomes.swap(m_outcomes);
    }
    for (const Outcome &outcome : std::as_const(outcomes)) {
        if (const auto *scan = std::get_if<ScanOutcome>(&outcome)) {
            scanFinished(*scan);
        } else if (const auto *progress = std::get_if<ProgressOutcome>(&outcome)) {
            if (m_scanning) {
                m_status.done = progress->done;
                m_status.total = progress->total;
                publish();
            }
        } else if (const auto *built = std::get_if<IndexOutcome>(&outcome)) {
            m_building = false;
            m_index = built->index;
            m_status.albums = built->albums;
            publish();
            Q_EMIT indexChanged(m_index);
            if (std::exchange(m_rebuildWanted, false)) {
                rebuild();
            }
        }
    }
}

void CollectionService::waitForIdle()
{
    for (int round = 0; round < 8; ++round) {
        m_pool.waitForDone();
        drain();
        if (m_rebuildTimer->isActive()) {
            m_rebuildTimer->stop();
            rebuild();
            continue;
        }
        if (!m_scanning && !m_building) {
            return;
        }
    }
}

void CollectionService::publish()
{
    Q_EMIT statusChanged(m_status);
}

} // namespace selecta::core
