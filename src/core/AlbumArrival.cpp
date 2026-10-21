// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/AlbumArrival.h"

#include "audio/Decoder.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "core/ArrivalService.h"
#include "core/InboxConversion.h"
#include "core/ReleaseJson.h"
#include "core/SearchResults.h"
#include "library/Quarantine.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

namespace keys = convert::keys;
using convert::Provenance;

constexpr qint64 kSameRecordingMs = 3000; // ±3 s: the same recording (D23)
constexpr double kMinFit = 0.5;           // below this, a file isn't taken for a track by its name

QString translated(const char *text)
{
    return QCoreApplication::translate("selecta::core::AlbumArrival", text);
}

bool isImage(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == u"jpg"_s || suffix == u"jpeg"_s || suffix == u"png"_s;
}

int numberOf(const QString &value)
{
    bool ok = false;
    const int number = value.section(u'/', 0, 0).trimmed().toInt(&ok);
    return ok && number > 0 ? number : 0;
}

// One audio file on its way in.
struct Source
{
    const AlbumArrivalFile *file = nullptr;
    convert::SourceQuality quality;
    convert::MetadataModel tags; // its own
    match::ParsedRemote parsed;
    qint64 durationMs = 0;
    int disc = 0;
    int track = 0;
    qsizetype releaseTrack = -1; // index into the job's tracks
    QString error;
};

// The folder image to use: one named like a cover, else the biggest.
QByteArray folderCover(const AlbumArrivalJob &job)
{
    const AlbumArrivalFile *best = nullptr;
    qint64 bestSize = -1;
    bool bestNamed = false;
    for (const AlbumArrivalFile &file : job.files) {
        if (!isImage(file.remoteName)) {
            continue;
        }
        const QString base = QFileInfo(file.remoteName).completeBaseName().toLower();
        const bool named = base == u"cover"_s || base == u"folder"_s || base == u"front"_s;
        const qint64 size = QFileInfo(file.localPath).size();
        if (!best || (named && !bestNamed) || (named == bestNamed && size > bestSize)) {
            best = &file;
            bestSize = size;
            bestNamed = named;
        }
    }
    if (!best) {
        return {};
    }
    QFile image(best->localPath);
    constexpr qint64 kMaxImageBytes = qint64{20} * 1024 * 1024;
    return image.size() <= kMaxImageBytes && image.open(QIODevice::ReadOnly) ? image.readAll() : QByteArray();
}

// Matches files to the album's tracks: by disc and track number when the lengths agree (or one is unknown), then
// the rest by how well their names fit, best pairs first. A track takes at most one file.
void matchTracks(QList<Source> &sources, const QList<meta::Release> &tracks)
{
    QSet<qsizetype> taken;
    for (Source &source : sources) {
        if (source.track <= 0) {
            continue;
        }
        for (qsizetype i = 0; i < tracks.size(); ++i) {
            const meta::Release &track = tracks.at(i);
            const int disc = track.discNumber.value_or(1);
            if (taken.contains(i) || track.trackNumber.value_or(0) != source.track ||
                (source.disc > 0 && disc != source.disc)) {
                continue;
            }
            const bool lengthsAgree = !track.durationMs || source.durationMs <= 0 ||
                                      std::abs(*track.durationMs - source.durationMs) <= kSameRecordingMs;
            if (lengthsAgree) {
                source.releaseTrack = i;
                taken.insert(i);
                break;
            }
        }
    }
    struct Candidate
    {
        double fit = 0.0;
        qsizetype source = 0;
        qsizetype track = 0;
    };
    QList<Candidate> candidates;
    for (qsizetype s = 0; s < sources.size(); ++s) {
        if (sources.at(s).releaseTrack >= 0) {
            continue;
        }
        for (qsizetype t = 0; t < tracks.size(); ++t) {
            if (taken.contains(t)) {
                continue;
            }
            const match::ReleaseFit fit =
                match::ReleaseScorer(referenceOf(tracks.at(t)))
                    .score(sources.at(s).parsed, static_cast<int>(sources.at(s).durationMs / 1000));
            if (fit.fit >= kMinFit) {
                candidates.append({.fit = fit.fit, .source = s, .track = t});
            }
        }
    }
    std::ranges::sort(candidates, [](const Candidate &a, const Candidate &b) { return a.fit > b.fit; });
    for (const Candidate &candidate : std::as_const(candidates)) {
        Source &source = sources[candidate.source];
        if (source.releaseTrack < 0 && !taken.contains(candidate.track)) {
            source.releaseTrack = candidate.track;
            taken.insert(candidate.track);
        }
    }
}

// The album's own fields, given to every file: from the release when there is one.
convert::MetadataModel albumFields(const AlbumArrivalJob &job)
{
    convert::MetadataModel fields;
    if (!job.release) {
        return fields;
    }
    const convert::MetadataModel release = tagsFromRelease(*job.release);
    for (const QStringView key :
         {keys::Album, keys::AlbumArtist, keys::Date, keys::Label, keys::CatalogNumber, keys::Genre}) {
        if (release.contains(key)) {
            fields.set(key, release.values(key), Provenance::Release);
        }
    }
    return fields;
}

} // namespace

namespace {

// Every audio file read: its quality, its own tags, its length and its numbers. The folder's file names together
// tell the track numbers apart (one pattern per folder).
QList<Source> readSources(const AlbumArrivalJob &job, const audio::CancellationFlag *cancel)
{
    QList<Source> sources;
    QStringList names;
    for (const AlbumArrivalFile &file : job.files) {
        if (match::RemotePathParser::fileKind(QFileInfo(file.remoteName).suffix()) == match::FileKind::Audio) {
            sources.append(Source{.file = &file});
            names.append(file.remoteName);
        }
    }
    const QList<match::ParsedRemote> parsed = match::RemotePathParser::parseFolder(job.folderName, names);
    for (qsizetype i = 0; i < sources.size(); ++i) {
        Source &source = sources[i];
        if (i < parsed.size()) {
            source.parsed = parsed.at(i);
        }
        convert::Result<convert::SourceQuality> quality =
            convert::probeSourceQuality(source.file->localPath, false, cancel);
        if (!quality) {
            source.error = translated("Selecta can't read this file: %1").arg(quality.error().message);
            continue;
        }
        source.quality = quality.takeValue();
        if (const audio::Result<audio::StreamInfo> info = audio::probeStreamInfo(source.file->localPath);
            info && info.value().durationSeconds() > 0) {
            source.durationMs = static_cast<qint64>(info.value().durationSeconds() * 1000.0);
        }
        if (convert::Result<convert::MetadataModel> own = convert::readTags(source.file->localPath); own) {
            source.tags = own.takeValue();
        }
        source.disc = numberOf(source.tags.value(keys::DiscNumber));
        source.track = numberOf(source.tags.value(keys::TrackNumber));
        if (source.track == 0 && source.parsed.track.isSet()) {
            source.track = source.parsed.track.value;
        }
        if (source.disc == 0 && source.parsed.disc.isSet()) {
            source.disc = source.parsed.disc.value;
        }
    }
    return sources;
}

// What the album folder is named after: the release's album fields, else the files' own, else the peer's folder.
library::naming::Fields folderFieldsOf(const AlbumArrivalJob &job, const convert::MetadataModel &album,
                                       const QList<Source> &sources)
{
    library::naming::Fields fields;
    fields.albumArtist = album.values(keys::AlbumArtist).join(u", "_s);
    fields.album = album.value(keys::Album);
    fields.year = album.value(keys::Date).left(4);
    const auto fill = [](QString &field, const QString &value) {
        if (field.isEmpty()) {
            field = value;
        }
    };
    for (const Source &source : sources) {
        fill(fields.album, source.tags.value(keys::Album));
        fill(fields.albumArtist, source.tags.value(keys::AlbumArtist));
        fill(fields.albumArtist, source.tags.values(keys::Artist).join(u", "_s));
        fill(fields.year, source.tags.value(keys::Date).left(4));
    }
    fill(fields.album, job.folderName);
    return fields;
}

// What converting the files gave.
struct Converted
{
    QList<AlbumArrivalResult::Track> tracks;
    QByteArray cover; // the processed cover, for cover.jpg
    QSet<QString> formats;
    QSet<int> bitrates;
};

// One audio file into the staged album folder, tagged.
AlbumArrivalResult::Track convertOne(const AlbumArrivalJob &job, const Source &source,
                                     const convert::MetadataModel &album, const QByteArray &cover,
                                     const QString &staging, Converted &converted,
                                     const audio::CancellationFlag *cancel)
{
    AlbumArrivalResult::Track track;
    track.downloadId = source.file->downloadId;
    if (!source.error.isEmpty()) {
        track.error = source.error;
        return track;
    }
    convert::MetadataModel tags = source.tags;
    // A file without tags of its own: what its name says.
    const auto fromName = [&tags](QStringView key, const QString &value) {
        if (!tags.contains(key) && !value.isEmpty()) {
            tags.set(key, value, Provenance::FileName);
        }
    };
    fromName(keys::Title, source.parsed.title.value);
    fromName(keys::Artist, source.parsed.artist.isSet() ? source.parsed.artist.value : source.parsed.albumArtist.value);
    fromName(keys::TrackNumber, source.track > 0 ? QString::number(source.track) : QString());
    fromName(keys::DiscNumber, source.disc > 0 ? QString::number(source.disc) : QString());
    tags.overrideFrom(album);
    if (source.releaseTrack >= 0) {
        tags.overrideFrom(tagsFromRelease(job.tracks.at(source.releaseTrack)));
        track.releaseTrack = static_cast<quint64>(source.releaseTrack) + 1;
    }
    attachCover(tags, cover);
    if (const std::optional<convert::Picture> processed = tags.frontCover(); processed && converted.cover.isEmpty()) {
        converted.cover = processed->data;
    }
    const InboxFormat format = inboxFormatFor(source.quality, QFileInfo(source.file->localPath).suffix());
    library::naming::Fields fields = namingFieldsOf(tags);
    if (fields.track.isEmpty() && source.track > 0) {
        fields.track = QString::number(source.track);
    }
    const QString name =
        library::naming::componentName(job.library.templates.albumTrack, fields, job.library.cleaner, format.extension);
    QString output = QDir(staging).filePath(name);
    for (int n = 2; QFileInfo::exists(output); ++n) { // two files named alike: both kept
        output = QDir(staging).filePath(QFileInfo(name).completeBaseName() + u" (%1)."_s.arg(n) + format.extension);
    }
    const convert::Result<convert::TranscodeReport> written =
        writeInboxFile(source.file->localPath, output, format, tags, job.encodedBy, cancel);
    if (!written) {
        track.error = translated("Conversion failed: %1").arg(written.error().message);
        return track;
    }
    // A quarantine attribute the download has survives the conversion (NFR-4); publishing gives every file one anyway.
    library::quarantine::copy(source.file->localPath, output);
    track.ok = true;
    track.artist = tags.values(keys::Artist).join(u", "_s);
    track.title = tags.value(keys::Title);
    track.durationMs = source.durationMs;
    track.sourceQuality = sourceQualityText(source.quality);
    track.sizeBytes = QFileInfo(output).size();
    converted.formats.insert(format.format);
    if (format.bitrate) {
        converted.bitrates.insert(*format.bitrate);
    }
    return track;
}

// The originals once the album is in: converted lossless files to the Trash (D11), the rest deleted.
QString disposeOfOriginals(const AlbumArrivalJob &job, const QList<Source> &sources)
{
    QString warning;
    for (const Source &source : sources) {
        const bool converted =
            source.error.isEmpty() && !inboxFormatFor(source.quality, QFileInfo(source.file->localPath).suffix()).kept;
        QString trashed;
        if (source.quality.lossless && converted) {
            if (!job.trash(source.file->localPath, &trashed)) {
                warning = translated("Some original FLAC files couldn't be moved to the Trash.");
            }
        } else {
            QFile::remove(source.file->localPath);
        }
    }
    for (const AlbumArrivalFile &file : job.files) {
        if (isImage(file.remoteName)) {
            QFile::remove(file.localPath);
        }
        QDir().rmdir(QFileInfo(file.localPath).absolutePath());
    }
    return warning;
}

void describeAlbum(AlbumArrivalResult &result, const AlbumArrivalJob &job, const library::naming::Fields &fields,
                   const Converted &converted, const QString &publishedPath, int arrived)
{
    library::InboxItem &item = result.item;
    item.kind = library::InboxItem::Kind::Album;
    const QString inboxRoot = QFileInfo(job.library.inboxRoot).canonicalFilePath();
    item.relativePath = QDir(inboxRoot.isEmpty() ? job.library.inboxRoot : inboxRoot)
                            .relativeFilePath(QFileInfo(publishedPath).canonicalFilePath());
    item.state = library::InboxItem::State::Ready;
    item.artist = fields.albumArtist;
    item.title = fields.album;
    item.album = fields.album;
    bool yearOk = false;
    if (const int year = fields.year.toInt(&yearOk); yearOk && year > 0) {
        item.year = year;
    }
    item.trackCount = arrived;
    for (const AlbumArrivalResult::Track &track : std::as_const(result.tracks)) {
        if (track.ok) {
            item.durationMs += track.durationMs;
            item.sizeBytes += track.sizeBytes;
            if (item.sourceFormat.isEmpty()) {
                item.sourceFormat = track.sourceQuality;
            }
        }
    }
    item.format = converted.formats.size() == 1 ? *converted.formats.cbegin() : translated("Mixed");
    if (converted.bitrates.size() == 1) {
        item.bitrate = *converted.bitrates.cbegin();
    }
    item.tagSource = tagSourceOf(job.release);
    item.releaseJson = job.release ? QString::fromUtf8(releaseToText(*job.release)) : QString();
    item.peer = job.peer;
}

} // namespace

AlbumArrivalResult arriveAlbum(const AlbumArrivalJob &job, const audio::CancellationFlag *cancel)
{
    AlbumArrivalResult result;
    const auto stop = [&result] {
        result.cancelled = true;
        result.error = translated("Cancelled");
        return result;
    };
    QList<Source> sources = readSources(job, cancel);
    if (cancel && cancel->isCancelled()) {
        return stop();
    }
    matchTracks(sources, job.tracks);

    convert::MetadataModel album = albumFields(job);
    const QByteArray cover = job.cover.isEmpty() ? folderCover(job) : job.cover;
    const library::naming::Fields folderFields = folderFieldsOf(job, album, sources);
    // Every file says the same album, so DJ software keeps them together.
    if (!album.contains(keys::Album) && !folderFields.album.isEmpty()) {
        album.set(keys::Album, folderFields.album, Provenance::FileName);
    }
    if (!album.contains(keys::AlbumArtist) && !folderFields.albumArtist.isEmpty()) {
        album.set(keys::AlbumArtist, folderFields.albumArtist, Provenance::FileName);
    }
    const QString folderName =
        library::naming::componentName(job.library.templates.albumFolder, folderFields, job.library.cleaner);
    const QString stagingRoot = QDir(job.stagingDir).filePath(u"album-%1"_s.arg(job.albumId));
    const QString staging = QDir(stagingRoot).filePath(folderName);
    QDir(stagingRoot).removeRecursively();
    if (!QDir().mkpath(staging)) {
        result.error = translated("Selecta can't write to %1.").arg(QDir::toNativeSeparators(staging));
        return result;
    }
    const auto cleanUp = [&stagingRoot] { QDir(stagingRoot).removeRecursively(); };

    Converted converted;
    for (const Source &source : std::as_const(sources)) {
        if (cancel && cancel->isCancelled()) {
            cleanUp();
            return stop();
        }
        converted.tracks.append(convertOne(job, source, album, cover, staging, converted, cancel));
    }
    // In track order, as the album plays.
    const auto position = [&sources](qint64 downloadId) {
        const auto found = std::ranges::find_if(
            sources, [downloadId](const Source &source) { return source.file->downloadId == downloadId; });
        return found == sources.cend() ? 0 : (found->disc * 1000) + found->track;
    };
    std::ranges::stable_sort(converted.tracks,
                             [&position](const AlbumArrivalResult::Track &a, const AlbumArrivalResult::Track &b) {
                                 return position(a.downloadId) < position(b.downloadId);
                             });
    result.tracks = converted.tracks;
    const auto arrived = static_cast<int>(
        std::ranges::count_if(result.tracks, [](const AlbumArrivalResult::Track &track) { return track.ok; }));
    if (arrived == 0) {
        cleanUp();
        result.error = result.tracks.isEmpty() ? translated("The folder held no audio file.")
                                               : translated("None of its files could be converted.");
        return result;
    }
    if (!converted.cover.isEmpty()) {
        QFile coverFile(QDir(staging).filePath(u"cover.jpg"_s));
        if (!coverFile.open(QIODevice::WriteOnly) || coverFile.write(converted.cover) != converted.cover.size()) {
            coverFile.remove(); // the cover is in every file's tags anyway
        }
    }

    // Into the Inbox, in one move, every file quarantined.
    const library::FileOps fileOps(job.library.inboxRoot, {}, job.trash);
    const library::FileResult published =
        fileOps.publishFolderIntoInbox(staging, QDir(kInboxAlbumsFolder.toString()).filePath(folderName));
    cleanUp();
    if (!published.ok) {
        result.error = translated("Couldn't put the album in your Inbox: %1").arg(published.error);
        return result;
    }
    const QString disposed = disposeOfOriginals(job, sources);
    describeAlbum(result, job, folderFields, converted, published.path, arrived);
    result.ok = true;
    const int lost = job.missing + static_cast<int>(sources.size()) - arrived;
    result.complete = lost == 0;
    QStringList warnings;
    if (lost > 0) {
        warnings.append(lost == 1 ? translated("1 file of the album couldn't be downloaded or converted.")
                                  : translated("%1 files of the album couldn't be downloaded or converted.").arg(lost));
    }
    if (!disposed.isEmpty()) {
        warnings.append(disposed);
    }
    result.warning = warnings.join(u' ');
    result.item.warning = result.warning;
    return result;
}

} // namespace selecta::core
