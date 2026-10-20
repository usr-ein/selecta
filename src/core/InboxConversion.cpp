// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxConversion.h"

#include "convert/ArtworkProcessor.h"

#include <QCoreApplication>

using namespace Qt::StringLiterals;

namespace selecta::core {

QString sourceQualityText(const convert::SourceQuality &quality)
{
    QString codec = audio::codecName(quality.codec);
    if (quality.codec == audio::Codec::Pcm) {
        codec = audio::containerName(quality.container); // "WAV", "AIFF"
    }
    if (quality.lossless && quality.bitDepth > 0 && quality.sampleRate > 0) {
        return u"%1 %2/%3"_s.arg(codec).arg(quality.bitDepth).arg(quality.sampleRate / 1000.0, 0, 'g', 3);
    }
    if (quality.bitrateKbps > 0) {
        return quality.constantBitrate ? u"%1 %2"_s.arg(codec).arg(quality.bitrateKbps)
                                       : u"%1 VBR %2"_s.arg(codec).arg(quality.bitrateKbps);
    }
    return codec;
}

InboxFormat inboxFormatFor(const convert::SourceQuality &quality, const QString &sourceSuffix)
{
    InboxFormat format;
    const convert::Advice advice = convert::advise(quality, format.settings, convert::AdviceContext::Filing);
    if (advice.action != convert::Action::Convert) {
        format.settings.format = convert::OutputFormat::KeepAudio;
        format.kept = true;
        format.extension = sourceSuffix.toLower();
        format.format = audio::codecName(quality.codec) == u"PCM"_s ? audio::containerName(quality.container)
                                                                    : audio::codecName(quality.codec);
        if (!quality.lossless && quality.bitrateKbps > 0) {
            format.bitrate = quality.bitrateKbps;
        }
        return format;
    }
    format.extension = convert::fileExtension(format.settings.format);
    format.format = u"MP3"_s;
    format.bitrate = format.settings.mp3.bitrateKbps;
    return format;
}

void attachCover(convert::MetadataModel &tags, const QByteArray &cover, QString *why)
{
    QByteArray picture = cover;
    convert::Provenance provenance = convert::Provenance::Release;
    if (const std::optional<convert::Picture> embedded = tags.frontCover(); picture.isEmpty() && embedded) {
        picture = embedded->data;
        provenance =
            embedded->provenance == convert::Provenance::Unknown ? convert::Provenance::Embedded : embedded->provenance;
    }
    tags.clearPictures();
    if (picture.isEmpty()) {
        return;
    }
    if (convert::Result<convert::ProcessedArtwork> artwork = convert::processArtwork(picture); artwork) {
        tags.setFrontCover(artwork.value().toPicture(provenance));
    } else if (why) {
        *why = artwork.error().message;
    }
}

convert::Result<convert::TranscodeReport> writeInboxFile(const QString &source, const QString &output,
                                                         const InboxFormat &format, const convert::MetadataModel &tags,
                                                         const QString &encodedBy,
                                                         const audio::CancellationFlag *cancel)
{
    convert::TranscodeRequest request;
    request.sourcePath = source;
    request.outputPath = output;
    request.settings = format.settings;
    request.tags = tags;
    request.policy.encodedBy = encodedBy;
    if (format.settings.format == convert::OutputFormat::Mp3) {
        request.policy.encoderSettings = convert::lameSettingsString(format.settings.mp3);
    }
    return convert::transcode(request, cancel);
}

} // namespace selecta::core
