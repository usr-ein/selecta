// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/detail/FileReader.h"

#include "audio/Decoder.h"
#include "convert/TagReader.h"
#include "match/RemotePathParser.h"

#include <QFileInfo>

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::collection::detail {

namespace {

match::Codec codecOf(const audio::StreamInfo &info)
{
    switch (info.codec) {
    case audio::Codec::Mp3:
        return match::Codec::Mp3;
    case audio::Codec::Aac:
        return match::Codec::Aac;
    case audio::Codec::Vorbis:
        return match::Codec::Vorbis;
    case audio::Codec::Opus:
        return match::Codec::Opus;
    case audio::Codec::Flac:
        return match::Codec::Flac;
    case audio::Codec::Alac:
        return match::Codec::Alac;
    case audio::Codec::Pcm:
        return info.container == audio::Container::Aiff ? match::Codec::Aiff : match::Codec::Wav;
    case audio::Codec::Mp2:
    case audio::Codec::Mp1:
    case audio::Codec::Other:
    case audio::Codec::Unknown:
        break;
    }
    const QString name = info.codecName.toLower();
    if (name.startsWith(u"wma")) {
        return match::Codec::Wma;
    }
    if (name == u"ape") {
        return match::Codec::Ape;
    }
    if (name == u"wavpack") {
        return match::Codec::WavPack;
    }
    return match::Codec::Unknown;
}

// The standard MPEG audio bitrate nearest to `kbps`: a CBR file's nominal rate from its average (the average counts the
// Xing/Info frame too, so it runs a little high on short files).
int nearestMpegBitrate(int kbps)
{
    static constexpr std::array<int, 18> kRates{8,  16,  24,  32,  40,  48,  56,  64,  80,
                                                96, 112, 128, 144, 160, 192, 224, 256, 320};
    int best = kbps;
    int distance = std::numeric_limits<int>::max();
    for (const int rate : kRates) {
        if (std::abs(rate - kbps) < distance) {
            best = rate;
            distance = std::abs(rate - kbps);
        }
    }
    return best;
}

// The longest genre kept: a group in the Collection view, never a paragraph.
constexpr qsizetype kMaxGenreChars = 100;

// The first non-empty GENRE value, trimmed and bounded: what the Collection view groups by (P-010). Values aren't split
// further: "Hip-Hop/Rap" and "Drum & Bass" are one genre each.
QString genreOf(const convert::MetadataModel &tags)
{
    const QStringList values = tags.values(convert::keys::Genre);
    for (const QString &value : values) {
        QString genre = value.simplified();
        if (genre.isEmpty()) {
            continue;
        }
        if (genre.size() > kMaxGenreChars) {
            genre.truncate(kMaxGenreChars);
            if (genre.back().isHighSurrogate()) {
                genre.chop(1); // never half a character
            }
        }
        return genre;
    }
    return {};
}

// Distinct non-empty values, joined with " & " (the matcher splits artist sets on it).
QString joinedValues(const convert::MetadataModel &tags, QStringView key)
{
    QStringList values;
    const QStringList raw = tags.values(key);
    for (const QString &value : raw) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty() && !values.contains(trimmed)) {
            values.append(trimmed);
        }
    }
    return values.join(u" & ");
}

void applyTags(const convert::MetadataModel &tags, ScannedFile &file)
{
    namespace keys = convert::keys;
    file.artist = joinedValues(tags, keys::Artist);
    file.title = tags.value(keys::Title).trimmed();
    file.album = tags.value(keys::Album).trimmed();
    file.albumArtist = joinedValues(tags, keys::AlbumArtist);
    file.trackNumber = leadingNumber(tags.value(keys::TrackNumber));
    file.discNumber = leadingNumber(tags.value(keys::DiscNumber));
    file.year = yearOf(tags.value(keys::Date));
    if (file.year == 0) {
        file.year = yearOf(tags.value(keys::OriginalDate));
    }
    file.isrc = tags.value(keys::Isrc).trimmed();
    file.genre = genreOf(tags);
}

// No title tag: the names come from the file and its folders, as for a search result (note §12, `meta_source`).
void applyNames(const QString &relativePath, const QStringList &siblings, ScannedFile &file)
{
    const match::ParsedRemote parsed = match::RemotePathParser::parse(relativePath, siblings);
    if (!parsed.title.isSet() || parsed.title.value.isEmpty()) {
        return;
    }
    file.namesFromFileName = true;
    file.title = parsed.title.value;
    if (file.artist.isEmpty() && parsed.artist.isSet()) {
        file.artist = parsed.artist.value;
    }
    if (file.album.isEmpty() && parsed.album.isSet()) {
        file.album = parsed.album.value;
    }
    if (file.albumArtist.isEmpty() && parsed.albumArtist.isSet()) {
        file.albumArtist = parsed.albumArtist.value;
    }
    if (file.trackNumber == 0 && parsed.track.isSet()) {
        file.trackNumber = parsed.track.value;
    }
    if (file.discNumber == 0 && parsed.disc.isSet()) {
        file.discNumber = parsed.disc.value;
    }
    if (file.year == 0 && parsed.year.isSet()) {
        file.year = parsed.year.value;
    }
}

} // namespace

int leadingNumber(QStringView text)
{
    constexpr int kMaxDigits = 4;
    text = text.trimmed();
    int value = 0;
    int digits = 0;
    for (const QChar c : text) {
        if (!c.isDigit() || c.unicode() > u'9' || digits == kMaxDigits) {
            break;
        }
        value = (value * 10) + (c.unicode() - u'0');
        ++digits;
    }
    return value;
}

int yearOf(QStringView date)
{
    constexpr int kYearDigits = 4;
    date = date.trimmed();
    if (date.size() < kYearDigits) {
        return 0;
    }
    for (qsizetype i = 0; i < kYearDigits; ++i) {
        if (date[i] < u'0' || date[i] > u'9') {
            return 0;
        }
    }
    if (date.size() > kYearDigits && date[kYearDigits].isDigit()) {
        return 0; // a longer number is not a year
    }
    const int year = leadingNumber(date.first(kYearDigits));
    return year >= 1000 ? year : 0;
}

match::AudioQuality qualityOf(const audio::StreamInfo &info)
{
    match::AudioQuality quality;
    quality.codec = codecOf(info);
    quality.sampleRateHz = info.sampleRate;
    quality.bitDepth = info.lossless ? info.bitsPerSample : 0;
    quality.bitrateKbps = info.bitrateKbps;
    // The probe's bitrate is the Xing/LAME tag's average over the stream when the tag has the counts, else the first
    // frame's (exact for CBR).
    if (info.lameTag) {
        const audio::LameTag &tag = *info.lameTag;
        // LAME's method when recorded (1 and 8: CBR), else Info (CBR) against Xing (VBR, ABR).
        const bool vbr = tag.vbrMethod > 0 ? (tag.vbrMethod != 1 && tag.vbrMethod != 8) : !tag.isInfo;
        quality.vbr = vbr;
        if (!vbr && info.codec == audio::Codec::Mp3) {
            quality.bitrateKbps = nearestMpegBitrate(info.bitrateKbps);
        }
    }
    return quality;
}

ReadOutcome readFile(const FileSignature &signature, const QString &relativePath, const QStringList &siblings,
                     const convert::TagReadOptions &tags)
{
    ReadOutcome outcome;
    // The header alone (tens of KB, never a decode): audio::probeStreamInfo opens the file read-only.
    const audio::Result<audio::StreamInfo> stream = audio::probeStreamInfo(signature.path);
    if (!stream.ok()) {
        if (!QFileInfo::exists(signature.path)) {
            outcome.status = ReadOutcome::Status::Vanished;
            return outcome;
        }
        outcome.reason = stream.error().toString();
        return outcome;
    }
    ScannedFile &file = outcome.file;
    file.file = signature;
    const audio::StreamInfo &info = stream.value();
    const double seconds = info.durationSeconds();
    file.durationMs = seconds > 0 ? std::llround(seconds * 1000.0) : 0;
    file.quality = qualityOf(info);
    // Tags that can't be read leave the names to the file and folders: the audio itself is fine (decision 5).
    if (const convert::Result<convert::MetadataModel> model = convert::readTags(signature.path, tags); model.ok()) {
        applyTags(model.value(), file);
    }
    if (file.title.isEmpty()) {
        applyNames(relativePath, siblings, file);
    }
    outcome.status = ReadOutcome::Status::Read;
    return outcome;
}

} // namespace selecta::collection::detail
