// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/detail/FileReader.h"

#include "audio/ByteSource.h"
#include "audio/Decoder.h"
#include "convert/TagReader.h"
#include "match/RemotePathParser.h"

#include <QFileInfo>

#include <cmath>
#include <memory>

using namespace Qt::StringLiterals;

namespace selecta::collection::detail {

namespace {

// A complete file presented as a download that has every byte but isn't finished: Decoder::open() then parses the
// container header (and the Xing/LAME frame or STREAMINFO) instead of indexing every frame, which would read the
// whole file (DESIGN.md decision 1).
class HeaderOnlySource final : public audio::ByteSource
{
public:
    explicit HeaderOnlySource(std::shared_ptr<audio::FileSource> file)
        : m_file(std::move(file))
    {}
    ~HeaderOnlySource() override = default;
    HeaderOnlySource(const HeaderOnlySource &) = delete;
    HeaderOnlySource &operator=(const HeaderOnlySource &) = delete;
    HeaderOnlySource(HeaderOnlySource &&) = delete;
    HeaderOnlySource &operator=(HeaderOnlySource &&) = delete;

    qint64 totalSize() const override { return m_file->totalSize(); }
    audio::SourceSnapshot snapshot() const override { return {m_file->totalSize(), audio::SourceState::Growing}; }
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override
    {
        const qint64 count = m_file->read(position, destination, maxBytes);
        if (count < 0) {
            setLastError(m_file->lastError());
        }
        return count;
    }
    QString description() const override { return m_file->description(); }

private:
    std::shared_ptr<audio::FileSource> m_file;
};

// The stream facts of `path`, from its header. The one place that knows how: when audio/ offers a header probe,
// this becomes a call to it.
audio::Result<audio::StreamInfo> probeStream(const QString &path)
{
    audio::Result<std::shared_ptr<audio::FileSource>> opened = audio::FileSource::open(path);
    if (!opened.ok()) {
        return opened.error();
    }
    const std::shared_ptr<audio::FileSource> file = opened.takeValue();
    const audio::DecoderOptions options{.sampleRate = 0, .channels = 0};
    audio::Decoder decoder;
    audio::Decoder::OpenStatus status = decoder.open(std::make_shared<HeaderOnlySource>(file), options);
    if (status == audio::Decoder::OpenStatus::NeedsCompleteFile || status == audio::Decoder::OpenStatus::NeedMoreData) {
        // A format only FFmpeg reads (MP4 without fast start, Ogg, WMA, …): its probe reads the header and a few
        // packets of the complete file.
        status = decoder.open(file, options);
    }
    if (status != audio::Decoder::OpenStatus::Ready) {
        return decoder.error();
    }
    return decoder.info();
}

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

// Samples per MPEG audio frame: Layer I 384, Layer II 1152, Layer III 1152 (MPEG-1) or 576 (MPEG-2 and 2.5).
int samplesPerMpegFrame(const audio::StreamInfo &info)
{
    if (!info.mpeg) {
        return 0;
    }
    switch (info.mpeg->layer) {
    case 1:
        return 384;
    case 2:
        return 1152;
    case 3:
        return info.mpeg->version == 1 ? 1152 : 576;
    default:
        return 0;
    }
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
    if (info.lameTag) {
        const audio::LameTag &tag = *info.lameTag;
        // LAME's method when recorded (1 and 8: CBR), else Info (CBR) against Xing (VBR, ABR).
        const bool vbr = tag.vbrMethod > 0 ? (tag.vbrMethod != 1 && tag.vbrMethod != 8) : !tag.isInfo;
        quality.vbr = vbr;
        // CBR keeps the frames' nominal rate; VBR takes the tag's average over the whole stream (delay and padding
        // included, as the byte count is).
        const int samplesPerFrame = samplesPerMpegFrame(info);
        if (vbr && tag.bytes > 0 && tag.frames > 0 && samplesPerFrame > 0 && info.sampleRate > 0) {
            const double seconds =
                static_cast<double>(tag.frames) * samplesPerFrame / static_cast<double>(info.sampleRate);
            quality.bitrateKbps =
                static_cast<int>(std::lround(static_cast<double>(tag.bytes) * 8.0 / seconds / 1000.0));
        }
    } else if (info.mpeg && info.mpeg->frameCount > 1) {
        quality.vbr = !info.mpeg->constantBitrate();
    }
    return quality;
}

ReadOutcome readFile(const FileSignature &signature, const QString &relativePath, const QStringList &siblings,
                     const convert::TagReadOptions &tags)
{
    ReadOutcome outcome;
    const audio::Result<audio::StreamInfo> stream = probeStream(signature.path);
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
