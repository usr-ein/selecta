// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ArrivalService.h"

#include "convert/ArtworkProcessor.h"
#include "convert/EncodeSettings.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/Transcoder.h"
#include "core/ActivityLog.h"
#include "core/InboxConversion.h"
#include "core/InboxService.h"
#include "core/MetaServices.h"
#include "core/ReleaseJson.h"
#include "core/SpectrumCheck.h"
#include "library/HistoryStore.h"
#include "library/Naming.h"
#include "library/Quarantine.h"
#include "storage/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcArrival, "selecta.core.arrival")

using convert::Provenance;
namespace keys = convert::keys;

QString numberOfTotal(std::optional<int> number, std::optional<int> total)
{
    if (!number || *number <= 0) {
        return {};
    }
    return total && *total >= *number ? u"%1/%2"_s.arg(*number).arg(*total) : QString::number(*number);
}

} // namespace

QString tagSourceOf(const std::optional<meta::Release> &release)
{
    if (!release) {
        return u"file"_s;
    }
    switch (release->source) {
    case meta::Source::Spotify:
        return u"spotify"_s;
    case meta::Source::Deezer:
        return u"deezer"_s;
    case meta::Source::MusicBrainz:
        return u"musicbrainz"_s;
    case meta::Source::CoverArtArchive:
    case meta::Source::YouTube:
        break;
    }
    return u"file"_s;
}

library::naming::Fields namingFieldsOf(const convert::MetadataModel &tags)
{
    library::naming::Fields fields;
    fields.artist = tags.values(keys::Artist).join(u", "_s);
    fields.albumArtist = tags.values(keys::AlbumArtist).join(u", "_s);
    fields.title = tags.value(keys::Title);
    fields.album = tags.value(keys::Album);
    fields.year = tags.value(keys::Date).left(4);
    fields.track = tags.value(keys::TrackNumber).section(u'/', 0, 0);
    fields.disc = tags.value(keys::DiscNumber).section(u'/', 0, 0);
    fields.discTotal = tags.value(keys::DiscNumber).section(u'/', 1, 1);
    fields.label = tags.value(keys::Label);
    fields.catalogNumber = tags.value(keys::CatalogNumber);
    fields.key = tags.value(keys::InitialKey);
    fields.bpm = tags.value(keys::Bpm);
    return fields;
}

convert::MetadataModel tagsFromRelease(const meta::Release &release)
{
    convert::MetadataModel tags;
    tags.set(keys::Title, release.title, Provenance::Release);
    tags.set(keys::Artist, release.artist(), Provenance::Release); // ", "-joined: the ID3v2.3 convention (D30)
    tags.set(keys::Album, release.album, Provenance::Release);
    tags.set(keys::AlbumArtist, release.albumArtist(), Provenance::Release);
    tags.set(keys::TrackNumber, numberOfTotal(release.trackNumber, release.trackTotal), Provenance::Release);
    tags.set(keys::DiscNumber, numberOfTotal(release.discNumber, release.discTotal), Provenance::Release);
    tags.set(keys::Date, release.releaseDate, Provenance::Release);
    tags.set(keys::Isrc, release.isrc, Provenance::Release);
    tags.set(keys::Label, release.label, Provenance::Release);
    tags.set(keys::CatalogNumber, release.catalogNumber, Provenance::Release);
    if (!release.genres.isEmpty()) {
        tags.set(keys::Genre, release.genres.constFirst(), Provenance::Release); // one genre, as DJ software shows it
    }
    const QUrl link = !release.spotifyUrl.isEmpty() ? release.spotifyUrl : release.deezerUrl;
    tags.set(keys::SourceUrl, link.toString(), Provenance::Release);
    return tags;
}

struct ArrivalService::Job
{
    Download download;
    std::optional<meta::Release> release; // completed by the lookups before the worker starts
    QByteArray cover;
    LibraryConfig library;
    QString stagingDir;
    QString encodedBy;
    library::FileOps::TrashFunction trash;
    std::shared_ptr<audio::AnalysisCache> cache;
    audio::CancellationFlag cancel;
    bool started = false; // the worker has it (engine thread only)
};

namespace {

// "6:12" for a length in ms.
QString minutesAndSeconds(qint64 milliseconds)
{
    const qint64 seconds = (milliseconds + 500) / 1000;
    return u"%1:%2"_s.arg(seconds / 60).arg(seconds % 60, 2, 10, u'0');
}

// DL-10: "It plays 3:12, not the 6:40 the search showed…" when the file's length is far from what was expected (the
// search result's, else the release's); empty when it's close enough or nothing was expected.
QString lengthMismatch(const std::optional<FileChoice> &file, const std::optional<meta::Release> &release,
                       double seconds)
{
    constexpr double kToleranceSeconds = 5.0; // search results truncate to whole seconds; versions differ by more
    qint64 expectedMs = 0;
    QString source;
    if (file && file->durationSec && *file->durationSec > 0) {
        expectedMs = qint64{*file->durationSec} * 1000;
        source = ArrivalService::tr("the search showed");
    } else if (release && release->durationMs && *release->durationMs > 0) {
        expectedMs = *release->durationMs;
        source = ArrivalService::tr("the release has");
    }
    if (expectedMs <= 0 || seconds <= 0.0 ||
        std::abs(seconds - (static_cast<double>(expectedMs) / 1000.0)) <= kToleranceSeconds) {
        return {};
    }
    return ArrivalService::tr("It plays %1, not the %2 %3: it may be cut short or another version.")
        .arg(minutesAndSeconds(static_cast<qint64>(seconds * 1000.0)), minutesAndSeconds(expectedMs), source);
}

} // namespace

struct ArrivalService::Outcome
{
    qint64 downloadId = 0;
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString warning;
    QString relativePath;
    library::InboxItem item; // the facts, without id
    QString sourceQuality;
};

ArrivalService::ArrivalService(storage::Database &db, InboxService &inbox, ActivityLog &activity,
                               library::FileOps::TrashFunction trash, Options options, WallClock now, QObject *parent)
    : QObject(parent)
    , m_db(db)
    , m_inbox(inbox)
    , m_activity(activity)
    , m_trash(trash ? std::move(trash) : library::FileOps::TrashFunction(&library::moveToSystemTrash))
    , m_options(std::move(options))
    , m_now(now ? std::move(now) : [] { return QDateTime::currentMSecsSinceEpoch(); })
{
    qRegisterMetaType<ArrivalResult>();
    const int workers =
        m_options.maxConcurrent > 0 ? m_options.maxConcurrent : std::clamp(QThread::idealThreadCount() / 2, 1, 4);
    m_pool.setMaxThreadCount(workers);
    m_pool.setObjectName(u"selecta-arrival"_s);
    // Leftovers of conversions a quit or a crash interrupted: every arrival starts its own folder afresh anyway.
    if (!m_options.stagingDir.isEmpty() && QFileInfo::exists(m_options.stagingDir) &&
        !QDir(m_options.stagingDir).removeRecursively()) {
        qCWarning(lcArrival) << "couldn't clear" << m_options.stagingDir;
    }
}

ArrivalService::~ArrivalService()
{
    for (const std::shared_ptr<Job> &job : std::as_const(m_jobs)) {
        job->cancel.cancel();
    }
    m_pool.waitForDone(); // workers delete their temporary files when cancelled
}

void ArrivalService::start(const Download &download)
{
    if (m_jobs.contains(download.id)) {
        return; // already arriving
    }
    const auto job = std::make_shared<Job>();
    job->download = download;
    job->release = download.release;
    job->library = m_inbox.config();
    job->stagingDir = m_options.stagingDir;
    job->encodedBy = m_options.encodedBy;
    job->trash = m_trash;
    job->cache = m_options.analysisCache;
    m_jobs.insert(download.id, job);
    // The lookups are bounded: past the timeout the file is tagged with what is known by then.
    QTimer::singleShot(m_options.metadataTimeoutMs, this, [this, weak = std::weak_ptr<Job>(job)] {
        if (const std::shared_ptr<Job> job = weak.lock()) {
            runWorker(job);
        }
    });
    lookUpTags(job);
}

void ArrivalService::cancel(qint64 downloadId)
{
    const std::shared_ptr<Job> job = m_jobs.value(downloadId);
    if (!job) {
        return;
    }
    job->cancel.cancel();
    if (!job->started) {
        m_jobs.remove(downloadId);
        Q_EMIT finished(downloadId, {false, 0, tr("Cancelled"), {}});
    } // else the worker stops at its next check and reports it
}

void ArrivalService::waitForIdle()
{
    m_pool.waitForDone();
    drain();
}

void ArrivalService::lookUpTags(const std::shared_ptr<Job> &job)
{
    const bool wantEnrichment = m_meta && job->release && !job->release->isrc.isEmpty() &&
                                (job->release->label.isEmpty() || job->release->genres.isEmpty());
    if (!wantEnrichment || !job->release) {
        fetchCover(job);
        return;
    }
    const meta::Release &release = *job->release;
    m_meta->musicBrainz()
        .enrich(release, meta::Priority::Background)
        .then(this, [this, weak = std::weak_ptr<Job>(job)](const meta::Result<meta::musicbrainz::Enrichment> &result) {
            const std::shared_ptr<Job> job = weak.lock();
            if (!job || job->started) {
                return; // cancelled, or the timeout went ahead without it
            }
            if (result.ok()) {
                job->release = meta::MusicBrainzClient::applied(*job->release, result.value());
            } else {
                qCInfo(lcArrival).noquote() << "no label or genre from MusicBrainz:" << result.error().message;
            }
            fetchCover(job);
        });
}

void ArrivalService::fetchCover(const std::shared_ptr<Job> &job)
{
    meta::CoverFetcher *covers = m_meta ? m_meta->covers() : nullptr;
    if (covers == nullptr || !job->release) {
        runWorker(job);
        return;
    }
    covers->fetch(*job->release, meta::Priority::Background)
        .then(this, [this, weak = std::weak_ptr<Job>(job)](const meta::Result<meta::CoverImage> &result) {
            const std::shared_ptr<Job> job = weak.lock();
            if (!job || job->started) {
                return;
            }
            if (result.ok()) {
                job->cover = result.value().bytes;
            } else {
                qCInfo(lcArrival).noquote() << "no cover:" << result.error().message;
            }
            runWorker(job);
        });
}

void ArrivalService::runWorker(const std::shared_ptr<Job> &job)
{
    if (job->started || !m_jobs.contains(job->download.id)) {
        return;
    }
    job->started = true;
    m_pool.start([this, job] {
        Outcome outcome = work(*job);
        {
            const std::scoped_lock lock(m_mutex);
            m_outcomes.append(std::move(outcome));
        }
        // By name: the request travels through Qt's own (uninstrumented) code, not in a queued functor.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    });
}

void ArrivalService::drain()
{
    QList<Outcome> outcomes;
    {
        const std::scoped_lock lock(m_mutex);
        outcomes.swap(m_outcomes);
    }
    for (const Outcome &outcome : std::as_const(outcomes)) {
        record(outcome);
    }
}

ArrivalService::Outcome ArrivalService::work(const Job &job)
{
    Outcome outcome;
    outcome.downloadId = job.download.id;
    const QString source = job.download.localPath;
    const QString stagingFolder = QDir(job.stagingDir).filePath(QString::number(job.download.id));
    const auto cleanUp = [&stagingFolder] { QDir(stagingFolder).removeRecursively(); };
    if (source.isEmpty() || !QFileInfo::exists(source)) {
        outcome.error = tr("The downloaded file is missing.");
        return outcome;
    }

    // What the file really is, and what the Inbox gets from it (D29: an MP3 below the format is kept as is;
    // other lossy formats become MP3, P-025).
    convert::Result<convert::SourceQuality> probed = convert::probeSourceQuality(source, false, &job.cancel);
    if (!probed) {
        outcome.cancelled = job.cancel.isCancelled();
        outcome.error = tr("Selecta can't read this file: %1").arg(probed.error().message);
        return outcome;
    }
    const convert::SourceQuality quality = probed.takeValue();
    outcome.sourceQuality = sourceQualityText(quality);
    const InboxFormat format = inboxFormatFor(quality, QFileInfo(source).suffix());
    QStringList warnings;
    if (format.belowFormat) {
        warnings.append(tr("Kept as %1: converting it to MP3 320 can't improve it.").arg(outcome.sourceQuality));
    }
    // PREV-9: is it what it claims? Checked on the original, before conversion; the findings inform, the format stays.
    const SpectrumCheck spectrum = checkSpectrum(source, job.cache.get(), &job.cancel);
    if (job.cancel.isCancelled()) {
        outcome.cancelled = true;
        outcome.error = tr("Cancelled");
        return outcome;
    }

    // Tags: the file's own, overridden by the release; the cover processed for CDJs.
    convert::MetadataModel tags;
    if (convert::Result<convert::MetadataModel> own = convert::readTags(source); own) {
        tags = own.takeValue();
    }
    if (job.release) {
        tags.overrideFrom(tagsFromRelease(*job.release));
    }
    QString coverProblem;
    attachCover(tags, job.cover, &coverProblem);
    if (!coverProblem.isEmpty()) {
        qCInfo(lcArrival).noquote() << "cover not used:" << coverProblem;
    }

    // The name it will have in the Inbox (DL-13).
    const QString name = library::naming::componentName(job.library.templates.single, namingFieldsOf(tags),
                                                        job.library.cleaner, format.extension);
    cleanUp();
    if (!QDir().mkpath(stagingFolder)) {
        outcome.error = tr("Selecta can't write to %1.").arg(QDir::toNativeSeparators(stagingFolder));
        return outcome;
    }
    const QString staged = QDir(stagingFolder).filePath(name);

    // Convert (or copy) and verify.
    const convert::Result<convert::TranscodeReport> report =
        writeInboxFile(source, staged, format, tags, job.encodedBy, &job.cancel);
    if (!report || job.cancel.isCancelled()) {
        cleanUp();
        outcome.cancelled = job.cancel.isCancelled();
        outcome.error = report ? tr("Cancelled") : tr("Conversion failed: %1").arg(report.error().message);
        return outcome;
    }
    // DL-10: it decoded whole; its length should be what the search (or the release) said.
    if (const QString mismatch =
            lengthMismatch(job.download.file, job.release, report.value().source.durationSeconds());
        !mismatch.isEmpty()) {
        warnings.append(mismatch);
    }
    // A quarantine attribute the download has survives the conversion (NFR-4); publishing gives it one otherwise.
    if (QString why; !library::quarantine::copy(source, staged, &why)) {
        qCInfo(lcArrival).noquote() << why;
    }

    // Into the Inbox, quarantined.
    const library::FileOps fileOps(job.library.inboxRoot, {}, job.trash);
    const library::FileResult published =
        fileOps.publishIntoInbox(staged, QDir(kInboxSinglesFolder.toString()).filePath(name));
    if (!published.ok) {
        cleanUp();
        outcome.error = tr("Couldn't put the file in your Inbox: %1").arg(published.error);
        return outcome;
    }

    // The original: a converted lossless file goes to the Trash (D11); anything else was copied, so it can go.
    if (quality.lossless && !format.kept) {
        QString trashed;
        if (!job.trash(source, &trashed)) {
            warnings.append(tr("The original FLAC couldn't be moved to the Trash; it is still in %1.")
                                .arg(QDir::toNativeSeparators(QFileInfo(source).absolutePath())));
        }
    } else if (!QFile::remove(source)) {
        qCWarning(lcArrival) << "couldn't delete the downloaded copy" << source;
    }
    QDir().rmdir(QFileInfo(source).absolutePath()); // the download's own folder, when empty
    cleanUp();

    const convert::TranscodeReport &done = report.value();
    library::InboxItem &item = outcome.item;
    item.kind = library::InboxItem::Kind::Single;
    // FileOps reports canonical paths (/private/var/… for /var/…): compare like with like.
    const QString inboxRoot = QFileInfo(job.library.inboxRoot).canonicalFilePath();
    item.relativePath = QDir(inboxRoot.isEmpty() ? job.library.inboxRoot : inboxRoot)
                            .relativeFilePath(QFileInfo(published.path).canonicalFilePath());
    item.state = library::InboxItem::State::Ready;
    item.artist = tags.values(keys::Artist).join(u", "_s);
    item.title = tags.value(keys::Title);
    item.album = tags.value(keys::Album);
    bool yearOk = false;
    if (const int year = tags.value(keys::Date).left(4).toInt(&yearOk); yearOk && year > 0) {
        item.year = year;
    }
    const double seconds = done.source.durationSeconds();
    item.durationMs = seconds > 0 ? static_cast<qint64>(seconds * 1000.0) : 0;
    item.format = format.format;
    item.bitrate = format.bitrate;
    item.sizeBytes = QFileInfo(published.path).size();
    item.sourceFormat = outcome.sourceQuality;
    item.tagSource = tagSourceOf(job.release);
    item.releaseJson = job.release ? QString::fromUtf8(releaseToText(*job.release)) : QString();
    item.peer = job.download.peer;
    item.remotePath = job.download.remotePath;
    item.warning = warnings.join(u' ');
    item.belowWanted = job.download.file ? job.download.file->shortfall : QString(); // got anyway (Get tracks)
    applySpectrumCheck(spectrum, item);
    outcome.warning = item.warning;
    outcome.relativePath = item.relativePath;
    outcome.ok = true;
    return outcome;
}

void ArrivalService::record(const Outcome &outcome)
{
    const std::shared_ptr<Job> job = m_jobs.take(outcome.downloadId);
    if (!job) {
        return;
    }
    const Download &download = job->download;
    if (!outcome.ok) {
        if (!outcome.cancelled) {
            m_activity.record(u"arrival"_s, download.peer, u"failed"_s,
                              download.remoteFileName() + u": "_s + outcome.error);
        }
        Q_EMIT finished(outcome.downloadId,
                        {false, 0, outcome.cancelled ? tr("Cancelled") : outcome.error, outcome.warning});
        return;
    }
    library::InboxItem item = outcome.item;
    item.session = m_options.session;
    item.createdAt = m_now();
    const std::optional<qint64> id = m_inbox.add(item);
    if (!id) {
        const QString error =
            tr("The file is in your Inbox folder, but Selecta couldn't record it: %1").arg(m_db.lastError());
        m_activity.record(u"arrival"_s, download.peer, u"failed"_s, error);
        Q_EMIT finished(outcome.downloadId, {false, 0, error, outcome.warning});
        return;
    }
    library::HistoryEntry entry;
    entry.session = m_options.session;
    entry.completedAt = item.createdAt;
    entry.kind = library::HistoryEntry::Kind::Single;
    entry.peer = download.peer;
    entry.remotePath = download.remotePath;
    entry.sizeBytes = download.size > 0 ? download.size : download.received;
    entry.quality = outcome.sourceQuality;
    entry.artist = item.artist;
    entry.title = item.title;
    entry.inboxItem = id;
    entry.whereabouts = library::HistoryEntry::Whereabouts::Inbox;
    if (!library::HistoryStore(m_db).add(entry)) {
        qCWarning(lcArrival) << "couldn't record the download in the history:" << m_db.lastError();
    }
    QStringList notes;
    if (const QString badge = spectrumBadge(item); !badge.isEmpty()) {
        notes.append(badge);
    }
    if (!outcome.warning.isEmpty()) {
        notes.append(outcome.warning);
    }
    m_activity.record(u"arrival"_s, download.peer, u"arrived"_s,
                      notes.isEmpty() ? outcome.relativePath
                                      : outcome.relativePath + u" ("_s + notes.join(u' ') + u')');
    Q_EMIT finished(outcome.downloadId, {true, *id, outcome.relativePath, outcome.warning});
}

} // namespace selecta::core
