// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/OutputVerifier.h"

#include "audio/ByteSource.h"
#include "convert/TagReader.h"
#include "convert/detail/PcmHash.h"
#include "convert/detail/TagLibUtil.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <cstdlib>
#include <taglib/infotag.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/wavfile.h>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr qint64 kChunkFrames = 8192;
constexpr int kMaxTagProblems = 8; // a wholly wrong tag would otherwise list every field
// The same bounds ArtworkProcessor applies before decoding an image (head implementer's limits).
constexpr int kMaxCoverSide = 10000;
constexpr qint64 kMaxCoverPixels = qint64{50} * 1000 * 1000;

TagReadOptions verifyReadOptions()
{
    TagReadOptions options;
    options.maxValueChars = qint64{1024} * 1024;
    options.maxLyricsChars = qint64{1024} * 1024;
    return options;
}

struct Decoded
{
    audio::StreamInfo info;
    audio::DecoderStats stats;
    qint64 frames = 0;
    QByteArray digest;
    QString stopReason; // why decoding ended before the end of the stream, if it did
};

// Decodes `path` completely at its own rate and channel count, hashing every sample.
Result<Decoded> decodeAndHash(const QString &path, const audio::CancellationFlag *cancel)
{
    Result<std::shared_ptr<audio::FileSource>> source = audio::FileSource::open(path);
    if (!source.ok()) {
        return source.error();
    }
    audio::DecoderOptions options;
    options.channels = 0; // the file's own layout and rate: nothing mixed, nothing resampled
    audio::Decoder decoder;
    switch (decoder.open(source.takeValue(), options)) {
    case audio::Decoder::OpenStatus::Ready:
        break;
    case audio::Decoder::OpenStatus::Failed:
        return decoder.error();
    case audio::Decoder::OpenStatus::NeedMoreData:
    case audio::Decoder::OpenStatus::NeedsCompleteFile:
        return makeError(ErrorCode::Internal, u"the decoder wants more of %1, which is complete"_s.arg(path));
    }
    Decoded decoded;
    decoded.info = decoder.info();
    const int channels = decoder.channels();
    std::vector<float> buffer(static_cast<std::size_t>(kChunkFrames * channels));
    QCryptographicHash hash(QCryptographicHash::Md5);
    while (true) {
        if (cancel != nullptr && cancel->isCancelled()) {
            return makeError(ErrorCode::Cancelled, u"cancelled"_s);
        }
        const audio::ReadResult read = decoder.read(buffer.data(), kChunkFrames);
        detail::addFloatSamples(hash, buffer.data(), read.frames * channels);
        decoded.frames += read.frames;
        if (read.status == audio::DecodeStatus::EndOfStream) {
            break;
        }
        if (read.status == audio::DecodeStatus::Error) {
            decoded.stopReason = decoder.error().message;
            break;
        }
        if (read.status == audio::DecodeStatus::NeedMoreData) {
            decoded.stopReason = u"the decoder wanted more data from a complete file"_s;
            break;
        }
    }
    decoded.stats = decoder.stats();
    decoded.digest = hash.result();
    return decoded;
}

QString shown(const QStringList &values)
{
    constexpr qsizetype kShownChars = 80;
    QString text = values.join(u" | "_s);
    if (text.size() > kShownChars) {
        text = text.left(kShownChars) + u'…';
    }
    return u"\"%1\""_s.arg(text);
}

void compareTags(const MetadataModel &expected, const MetadataModel &actual, QStringList &problems)
{
    int reported = 0;
    const auto report = [&](const QString &problem) {
        if (reported++ < kMaxTagProblems) {
            problems.append(problem);
        }
    };
    const QMap<QString, MetadataModel::Field> &want = expected.fields();
    for (auto it = want.cbegin(); it != want.cend(); ++it) {
        if (!actual.contains(it.key())) {
            report(u"tag %1 is missing"_s.arg(it.key()));
        } else if (actual.values(it.key()) != it.value().values) {
            report(u"tag %1 reads back as %2, not %3"_s.arg(it.key(), shown(actual.values(it.key())),
                                                            shown(it.value().values)));
        }
    }
    const QMap<QString, MetadataModel::Field> &have = actual.fields();
    for (auto it = have.cbegin(); it != have.cend(); ++it) {
        if (!expected.contains(it.key())) {
            report(u"unexpected tag %1 = %2"_s.arg(it.key(), shown(it.value().values)));
        }
    }
    if (reported > kMaxTagProblems) {
        problems.append(u"… and %1 more tag differences"_s.arg(reported - kMaxTagProblems));
    }
}

void checkCover(const MetadataModel &actual, const VerifyExpectations &expectations, QStringList &problems)
{
    const QList<Picture> &pictures = actual.pictures();
    if (pictures.size() != 1) {
        problems.append(u"%1 embedded pictures, expected one front cover"_s.arg(pictures.size()));
        return;
    }
    const Picture &cover = pictures.first();
    if (!cover.isFrontCover()) {
        problems.append(u"the embedded picture is of type %1, not a front cover"_s.arg(cover.type));
    }
    QBuffer buffer;
    buffer.setData(cover.data);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > kMaxCoverSide || size.height() > kMaxCoverSide ||
        qint64{size.width()} * size.height() > kMaxCoverPixels) {
        problems.append(u"the embedded cover isn't a readable image"_s);
        return;
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        problems.append(u"the embedded cover doesn't decode: %1"_s.arg(reader.errorString()));
    } else if (expectations.expectedCoverSize.isValid() && image.size() != expectations.expectedCoverSize) {
        problems.append(u"the embedded cover is %1×%2, expected %3×%4"_s.arg(image.width())
                            .arg(image.height())
                            .arg(expectations.expectedCoverSize.width())
                            .arg(expectations.expectedCoverSize.height()));
    }
}

// RIFF INFO is all rekordbox reads from a WAV file: it must carry the title and an artist.
void checkWavInfo(const QString &path, const MetadataModel &expected, QStringList &problems)
{
    const QByteArray nativePath = QFile::encodeName(path);
    TagLib::FileStream stream(nativePath.constData(), /*openReadOnly=*/true);
    const TagLib::RIFF::WAV::File file(&stream, /*readProperties=*/false);
    const TagLib::PropertyMap info =
        file.isValid() && file.hasInfoTag() ? file.InfoTag()->properties() : TagLib::PropertyMap();
    const QString title = info.contains("TITLE") ? detail::fromTagLib(info["TITLE"].front()) : QString();
    if (expected.contains(keys::Title) && title != expected.value(keys::Title)) {
        problems.append(
            u"the RIFF INFO title is %1, not %2"_s.arg(shown({title}), shown(expected.values(keys::Title))));
    }
    if (expected.contains(keys::Artist) && (!info.contains("ARTIST") || info["ARTIST"].isEmpty())) {
        problems.append(u"the RIFF INFO list has no artist"_s);
    }
}

void checkStream(const Decoded &decoded, const VerifyExpectations &expectations, QStringList &problems)
{
    const audio::StreamInfo &info = decoded.info;
    if (expectations.container && info.container != *expectations.container) {
        problems.append(u"the file is %1, expected %2"_s.arg(audio::containerName(info.container),
                                                             audio::containerName(*expectations.container)));
    }
    if (expectations.sampleRate > 0 && info.sampleRate != expectations.sampleRate) {
        problems.append(u"%1 Hz, expected %2 Hz"_s.arg(info.sampleRate).arg(expectations.sampleRate));
    }
    if (expectations.channels > 0 && info.channels != expectations.channels) {
        problems.append(u"%1 channels, expected %2"_s.arg(info.channels).arg(expectations.channels));
    }
    if (expectations.bitsPerSample > 0 && info.bitsPerSample != expectations.bitsPerSample) {
        problems.append(u"%1-bit, expected %2-bit"_s.arg(info.bitsPerSample).arg(expectations.bitsPerSample));
    }
    if (!decoded.stopReason.isEmpty()) {
        problems.append(u"decoding stopped after %1 frames: %2"_s.arg(decoded.frames).arg(decoded.stopReason));
    }
    if (expectations.requireCleanDecode) {
        if (decoded.stats.decodeErrors > 0) {
            problems.append(u"%1 packets didn't decode"_s.arg(decoded.stats.decodeErrors));
        }
        if (decoded.stats.skippedBytes > 0) {
            problems.append(u"%1 bytes of junk between audio frames"_s.arg(decoded.stats.skippedBytes));
        }
        if (decoded.stats.truncated) {
            problems.append(u"the audio ends before its header says it should"_s);
        }
    }
    if (expectations.expectedFrames >= 0 &&
        std::abs(decoded.frames - expectations.expectedFrames) > expectations.frameTolerance) {
        problems.append(u"decodes to %1 frames, expected %2 (± %3)"_s.arg(decoded.frames)
                            .arg(expectations.expectedFrames)
                            .arg(expectations.frameTolerance));
    }
    if (!expectations.expectedPcmDigest.isEmpty() && decoded.digest != expectations.expectedPcmDigest) {
        problems.append(u"the decoded audio isn't identical to the source's"_s);
    }
}

void checkMpeg(const audio::StreamInfo &info, const VerifyExpectations &expectations, QStringList &problems)
{
    if (expectations.mp3CbrKbps > 0) {
        if (!info.mpeg || info.mpeg->frameCount == 0) {
            problems.append(u"no MPEG audio frames"_s);
        } else if (info.mpeg->minBitrateKbps != expectations.mp3CbrKbps ||
                   info.mpeg->maxBitrateKbps != expectations.mp3CbrKbps) {
            problems.append(u"MPEG frames from %1 to %2 kbps, expected all at %3 kbps"_s.arg(info.mpeg->minBitrateKbps)
                                .arg(info.mpeg->maxBitrateKbps)
                                .arg(expectations.mp3CbrKbps));
        }
    }
    if (expectations.requireLameTag) {
        if (!info.lameTag) {
            problems.append(u"no Info/Xing header in the first frame"_s);
        } else if (!info.lameTag->hasLameExtension() || !info.lameTag->encoder.startsWith(u"LAME"_s)) {
            problems.append(u"the Info header wasn't written by LAME (encoder \"%1\")"_s.arg(info.lameTag->encoder));
        } else if (!info.lameTag->crcValid) {
            problems.append(u"the LAME tag's CRC doesn't match"_s);
        }
    }
}

} // namespace

Result<QByteArray> pcmDigest(const QString &path, const audio::CancellationFlag *cancel)
{
    Result<Decoded> decoded = decodeAndHash(path, cancel);
    if (!decoded.ok()) {
        return decoded.error();
    }
    if (!decoded.value().stopReason.isEmpty()) {
        return makeError(ErrorCode::DecoderError, u"decoding %1 stopped after %2 frames: %3"_s.arg(path)
                                                      .arg(decoded.value().frames)
                                                      .arg(decoded.value().stopReason));
    }
    return decoded.value().digest;
}

Result<VerifyReport> verifyOutput(const QString &path, const VerifyExpectations &expectations,
                                  const audio::CancellationFlag *cancel)
{
    if (!QFileInfo(path).isFile()) {
        return makeError(ErrorCode::OpenFailed, u"%1 is not a file"_s.arg(path));
    }
    Result<Decoded> decoded = decodeAndHash(path, cancel);
    if (!decoded.ok()) {
        return decoded.error();
    }
    const Decoded &audio = decoded.value();
    VerifyReport report;
    report.info = audio.info;
    report.stats = audio.stats;
    report.decodedFrames = audio.frames;
    report.pcmDigest = audio.digest;
    checkStream(audio, expectations, report.problems);
    checkMpeg(audio.info, expectations, report.problems);

    if (expectations.expectedTags || expectations.expectFrontCover) {
        const Result<MetadataModel> tags = readTags(path, verifyReadOptions());
        if (!tags.ok()) {
            report.problems.append(u"the tags can't be read: %1"_s.arg(tags.error().message));
        } else {
            if (expectations.expectedTags) {
                compareTags(*expectations.expectedTags, tags.value(), report.problems);
            }
            if (expectations.expectFrontCover) {
                checkCover(tags.value(), expectations, report.problems);
            }
        }
        if (expectations.expectedTags && audio.info.container == audio::Container::Wav) {
            checkWavInfo(path, *expectations.expectedTags, report.problems);
        }
    }
    return report;
}

} // namespace selecta::convert
