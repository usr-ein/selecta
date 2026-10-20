// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli media-convert <in> <out> [options]: convert a local file the way Selecta files it (decode, encode, tag,
// verify, rename without replacing), print the advice and the verification. The output format follows the output's
// extension. Touches no network.
#include "Command.h"
#include "convert/ArtworkProcessor.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/Transcoder.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstdio>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using selecta::cli::printLine;

namespace {

constexpr qint64 kMaxCoverFileBytes = qint64{20} * 1024 * 1024;

struct Options
{
    QString input;
    QString output;
    EncodeSettings settings;
    QList<std::pair<QString, QString>> tags; // key, value (from --title, --artist, … and --tag KEY=VALUE)
    QString coverPath;
    bool check = false; // run the quality checks and print the verdict
    bool yes = false;   // proceed when the advisor asks for confirmation
    bool id3v24 = false;
};

std::optional<OutputFormat> formatForExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"mp3"_s) {
        return OutputFormat::Mp3;
    }
    if (suffix == u"aiff"_s || suffix == u"aif"_s) {
        return OutputFormat::Aiff;
    }
    if (suffix == u"flac"_s) {
        return OutputFormat::Flac;
    }
    if (suffix == u"wav"_s) {
        return OutputFormat::Wav;
    }
    if (suffix == u"m4a"_s) {
        return OutputFormat::Alac;
    }
    return std::nullopt;
}

bool isNumberOption(const QString &argument)
{
    return argument == u"--bitrate"_s || argument == u"--vbr"_s || argument == u"--bits"_s || argument == u"--rate"_s;
}

void applyNumber(const QString &argument, int number, EncodeSettings &settings)
{
    if (argument == u"--bitrate"_s) {
        settings.mp3.mode = Mp3Mode::Cbr;
        settings.mp3.bitrateKbps = number;
    } else if (argument == u"--vbr"_s) {
        settings.mp3.mode = Mp3Mode::Vbr;
        settings.mp3.vbrQuality = number;
    } else if (argument == u"--bits"_s) {
        settings.bitDepth = number;
    } else {
        settings.sampleRate = number;
    }
}

QString verdictName(QualityVerdict::Kind kind)
{
    switch (kind) {
    case QualityVerdict::Kind::Genuine:
        return u"genuine"_s;
    case QualityVerdict::Kind::Suspect:
        return u"suspect"_s;
    case QualityVerdict::Kind::Unchecked:
        break;
    }
    return u"unchecked"_s;
}

// Parses the arguments; returns an error message, or an empty string.
QString parse(const QStringList &arguments, Options &options)
{
    bool keepAudio = false;
    QStringList positional;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString &argument = arguments[i];
        const auto value = [&](QString &out) {
            if (i + 1 >= arguments.size()) {
                return false;
            }
            out = arguments[++i];
            return true;
        };
        QString text;
        if (argument == u"--keep-audio"_s) {
            keepAudio = true;
        } else if (argument == u"--check"_s) {
            options.check = true;
        } else if (argument == u"--yes"_s) {
            options.yes = true;
        } else if (argument == u"--id3v24"_s) {
            options.id3v24 = true;
        } else if (argument == u"--cover"_s) {
            if (!value(options.coverPath)) {
                return u"--cover needs an image file"_s;
            }
        } else if (isNumberOption(argument)) {
            bool ok = false;
            const int number = value(text) ? text.toInt(&ok) : 0;
            if (!ok) {
                return u"%1 needs a number"_s.arg(argument);
            }
            applyNumber(argument, number, options.settings);
        } else if (argument == u"--tag"_s) {
            if (!value(text) || !text.contains(u'=')) {
                return u"--tag needs KEY=VALUE"_s;
            }
            options.tags.append({text.section(u'=', 0, 0), text.section(u'=', 1)});
        } else if (argument.startsWith(u"--"_s) && argument.size() > 2) {
            // --title, --artist, --album, --genre, --label, … : the TagLib property of the same name.
            if (!value(text)) {
                return u"%1 needs a value"_s.arg(argument);
            }
            options.tags.append({argument.mid(2).toUpper(), text});
        } else {
            positional.append(argument);
        }
    }
    if (positional.size() != 2) {
        return u"expected an input and an output file"_s;
    }
    options.input = positional[0];
    options.output = positional[1];
    if (keepAudio) {
        options.settings.format = OutputFormat::KeepAudio;
    } else if (const std::optional<OutputFormat> format = formatForExtension(options.output)) {
        options.settings.format = *format;
    } else {
        return u"the output's extension decides the format: .mp3, .aiff, .flac, .wav or .m4a (or --keep-audio)"_s;
    }
    return {};
}

// The cover to embed: --cover, else the source's own front cover, through the artwork processor either way.
std::optional<Picture> chooseCover(const Options &options, const MetadataModel &sourceTags)
{
    QByteArray bytes;
    Provenance provenance = Provenance::Embedded;
    if (!options.coverPath.isEmpty()) {
        QFile file(options.coverPath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxCoverFileBytes) {
            printLine(stderr, u"can't use the cover %1"_s.arg(options.coverPath));
            return std::nullopt;
        }
        bytes = file.readAll();
        provenance = Provenance::User;
    } else if (const std::optional<Picture> embedded = sourceTags.frontCover()) {
        bytes = embedded->data;
    } else {
        return std::nullopt;
    }
    const Result<ProcessedArtwork> processed = processArtwork(bytes);
    if (!processed.ok()) {
        printLine(stderr, u"cover skipped: "_s + processed.error().toString());
        return std::nullopt;
    }
    printLine(stdout, u"cover      %1×%2 JPEG, %3 bytes%4"_s.arg(processed.value().size.width())
                          .arg(processed.value().size.height())
                          .arg(processed.value().data.size())
                          .arg(processed.value().reencoded ? u", re-encoded"_s : u", kept as it is"_s));
    return processed.value().toPicture(provenance);
}

QString actionName(Action action)
{
    switch (action) {
    case Action::Convert:
        return u"convert"_s;
    case Action::KeepAsIs:
        return u"keep as it is (no re-encode)"_s;
    case Action::Ask:
        return u"ask: needs confirmation (--yes)"_s;
    case Action::Refuse:
        return u"refused"_s;
    }
    return {};
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    Options options;
    if (const QString problem = parse(arguments, options); !problem.isEmpty()) {
        printLine(stderr, problem);
        printLine(stderr,
                  u"usage: selecta-cli media-convert <in> <out.mp3|.aiff|.flac|.wav|.m4a> [--bitrate N | --vbr N] "
                  "[--bits 16|24] [--rate Hz] [--title T] [--artist A] [--KEY value] [--tag KEY=VALUE] "
                  "[--cover image] [--keep-audio] [--id3v24] [--check] [--yes]"_s);
        return selecta::cli::kExitUsage;
    }

    const Result<SourceQuality> quality = probeSourceQuality(options.input, options.check);
    if (!quality.ok()) {
        printLine(stderr, quality.error().toString());
        return selecta::cli::kExitFailed;
    }
    printLine(stdout, u"source     %1"_s.arg(describeSource(quality.value())));
    if (options.check) {
        const QualityVerdict verdict = judgeQuality(quality.value());
        printLine(stdout, u"verdict    %1: %2"_s.arg(verdictName(verdict.kind), verdict.reason));
    }
    const Advice advice = advise(quality.value(), options.settings, AdviceContext::Manual);
    printLine(stdout, u"target     %1 → %2"_s.arg(describeSettings(options.settings), actionName(advice.action)));
    for (const Notice notice : advice.notices) {
        const NoticeText text = describeNotice(notice, quality.value(), options.settings, advice);
        printLine(stdout, u"  · %1: %2"_s.arg(text.shortText, text.detail));
    }
    if (advice.action == Action::Refuse || (advice.action == Action::Ask && !options.yes)) {
        return selecta::cli::kExitFailed;
    }

    TranscodeRequest request;
    request.sourcePath = options.input;
    request.outputPath = options.output;
    request.settings = options.settings;
    request.policy.encodedBy = u"Selecta"_s;
    request.tagOptions.id3Version = options.id3v24 ? Id3Version::V24 : Id3Version::V23;
    const Result<MetadataModel> sourceTags = readTags(options.input);
    if (sourceTags.ok()) {
        request.tags = sourceTags.value();
    }
    request.tags.clearPictures();
    for (const auto &[key, value] : std::as_const(options.tags)) {
        request.tags.set(key, value, Provenance::User);
    }
    if (const std::optional<Picture> cover =
            chooseCover(options, sourceTags.ok() ? sourceTags.value() : MetadataModel{})) {
        request.tags.setFrontCover(*cover);
    }

    QElapsedTimer timer;
    timer.start();
    const Result<TranscodeReport> result = transcode(request);
    if (!result.ok()) {
        printLine(stderr, result.error().toString());
        return selecta::cli::kExitFailed;
    }
    const TranscodeReport &report = result.value();
    QString audio = u"%1 Hz"_s.arg(report.outputSampleRate);
    if (report.outputBitDepth > 0) {
        audio += u", %1-bit"_s.arg(report.outputBitDepth);
    }
    if (report.resampled) {
        audio += u", resampled"_s;
    }
    if (report.dithered) {
        audio += u", dithered"_s;
    }
    printLine(stdout, u"output     %1 (%2 bytes, %3)"_s.arg(report.outputPath).arg(report.outputBytes).arg(audio));
    const selecta::audio::StreamInfo &out = report.verification.info;
    if (out.lameTag) {
        printLine(stdout, u"lame tag   %1 \"%2\", delay %3, padding %4, CRC %5"_s
                              .arg(out.lameTag->isInfo ? u"Info"_s : u"Xing"_s, out.lameTag->encoder)
                              .arg(out.lameTag->encoderDelay)
                              .arg(out.lameTag->encoderPadding)
                              .arg(out.lameTag->crcValid ? u"valid"_s : u"invalid"_s));
    }
    printLine(stdout, u"verified   %1 frames decoded, PCM MD5 %2, tags and cover read back (%3 ms in all)"_s
                          .arg(report.verification.decodedFrames)
                          .arg(QString::fromLatin1(report.verification.pcmDigest.toHex()))
                          .arg(timer.elapsed()));
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"media-convert", u"<in> <out.mp3|.aiff|.flac|.wav|.m4a> [--title T --artist A --cover img …]",
    u"Convert, tag and verify a local audio file as Selecta files it.", &run};

} // namespace
