// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli media-analyze <file>: decode and analyse a local audio file; print the stream facts, a 3-band waveform
// summary and how long the analysis took. Touches no network.
#include "Command.h"
#include "audio/AnalysisCache.h"
#include "audio/Decoder.h"
#include "audio/WaveformAnalyzer.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <array>
#include <cstdio>

using namespace Qt::StringLiterals;
using namespace selecta::audio;

namespace {

QString yesNo(bool value)
{
    return value ? u"yes"_s : u"no"_s;
}

QString describeStream(const StreamInfo &info, qint64 lengthFrames)
{
    QString text =
        u"format     %1 / %2 (%3)\n"_s.arg(containerName(info.container), codecName(info.codec), info.codecName);
    text += u"audio      %1 Hz, %2 ch"_s.arg(info.sampleRate).arg(info.channels);
    if (info.bitsPerSample > 0) {
        text += u", %1-bit"_s.arg(info.bitsPerSample);
    }
    text += info.lossless ? u", lossless\n"_s : u", lossy\n"_s;
    const double seconds =
        info.sampleRate > 0 && lengthFrames >= 0 ? static_cast<double>(lengthFrames) / info.sampleRate : -1;
    text += u"length     %1 s (%2 frames, %3)\n"_s.arg(seconds, 0, 'f', 3)
                .arg(lengthFrames)
                .arg(info.exactLength ? u"exact"_s : u"estimated"_s);
    text += u"bitrate    %1 kbps\n"_s.arg(info.bitrateKbps);
    text += u"streaming  progressive %1, exact seeks %2\n"_s.arg(yesNo(info.progressive), yesNo(info.exactSeek));
    if (info.mpeg) {
        text += u"mpeg       layer %1, %2 frames, %3–%4 kbps (%5), %6 bytes skipped\n"_s.arg(info.mpeg->layer)
                    .arg(info.mpeg->frameCount)
                    .arg(info.mpeg->minBitrateKbps)
                    .arg(info.mpeg->maxBitrateKbps)
                    .arg(info.mpeg->constantBitrate() ? u"CBR"_s : u"VBR"_s)
                    .arg(info.mpeg->skippedBytes);
    }
    if (info.lameTag) {
        const LameTag &tag = *info.lameTag;
        text +=
            u"lame tag   %1 \"%2\", lowpass %3 Hz, delay %4, padding %5, CRC %6\n"_s
                .arg(tag.isInfo ? u"Info"_s : u"Xing"_s, tag.encoder.isEmpty() ? u"(no LAME extension)"_s : tag.encoder)
                .arg(tag.lowpassHz)
                .arg(tag.encoderDelay)
                .arg(tag.encoderPadding)
                .arg(tag.crcValid ? u"valid"_s : u"invalid"_s);
    }
    return text;
}

// The overview as a line of block characters (height = the sum of the three bands, like Pioneer's stacked view).
QString sparkline(const Waveform &waveform, int width)
{
    static const std::array<QChar, 9> blocks = {u' ',          QChar(0x2581), QChar(0x2582),
                                                QChar(0x2583), QChar(0x2584), QChar(0x2585),
                                                QChar(0x2586), QChar(0x2587), QChar(0x2588)};
    const QList<OverviewColumn> &overview = waveform.overview();
    QString line;
    if (overview.isEmpty()) {
        return line;
    }
    for (int x = 0; x < width; ++x) {
        const qsizetype begin = (x * overview.size()) / width;
        const qsizetype end = std::max(begin + 1, ((x + 1) * overview.size()) / width);
        int peak = 0;
        for (qsizetype i = begin; i < end && i < overview.size(); ++i) {
            const OverviewColumn &c = overview[i];
            peak = std::max(peak, std::min(255, (c.peak[0] + c.peak[1] + c.peak[2]) / 2));
        }
        line += blocks.at(static_cast<std::size_t>((peak * 8) / 255));
    }
    return line;
}

QString bandSummary(const Waveform &waveform)
{
    const std::array<QString, kBandCount> names = {u"low "_s, u"mid "_s, u"high"_s};
    QString text;
    for (std::size_t b = 0; b < kBandCount; ++b) {
        int maxPeak = 0;
        qint64 sum = 0;
        for (const WaveformColumn &column : waveform.detail()) {
            maxPeak = std::max<int>(maxPeak, column.peak.at(b));
            sum += column.rms.at(b);
        }
        const double meanRms = waveform.detail().isEmpty()
                                   ? 0.0
                                   : static_cast<double>(sum) / static_cast<double>(waveform.detail().size());
        text += u"  %1  peak max %2/255, mean RMS %3/255\n"_s.arg(names.at(b)).arg(maxPeak).arg(meanRms, 0, 'f', 1);
    }
    return text;
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    using selecta::cli::printLine;
    if (arguments.size() != 1) {
        printLine(stderr, u"usage: selecta-cli media-analyze <file>"_s);
        return selecta::cli::kExitUsage;
    }
    const QString &path = arguments.first();
    Result<std::shared_ptr<FileSource>> opened = FileSource::open(path);
    if (!opened.ok()) {
        printLine(stderr, opened.error().toString());
        return selecta::cli::kExitFailed;
    }
    const std::shared_ptr<FileSource> source = opened.takeValue();
    printLine(stdout, u"file       %1 (%2 bytes)"_s.arg(QFileInfo(path).fileName()).arg(source->totalSize()));

    QElapsedTimer timer;
    timer.start();
    Decoder decoder;
    if (decoder.open(source) != Decoder::OpenStatus::Ready) {
        printLine(stderr, u"can't decode: "_s + decoder.error().toString());
        return selecta::cli::kExitFailed;
    }
    const qint64 openMs = timer.elapsed();
    printLine(stdout, describeStream(decoder.info(), decoder.info().totalFrames).trimmed());
    printLine(stdout, u"open       %1 ms (header parse and frame index)"_s.arg(openMs));

    timer.restart();
    const Result<Waveform> waveform = analyzeWaveform(source);
    const qint64 analysisMs = timer.elapsed();
    if (!waveform.ok()) {
        printLine(stderr, u"analysis failed: "_s + waveform.error().toString());
        return selecta::cli::kExitFailed;
    }
    printLine(stdout, u"analysis   %1 ms, %2 columns at %3/s, %4 frames"_s.arg(analysisMs)
                          .arg(waveform.value().detail().size())
                          .arg(kColumnsPerSecond)
                          .arg(waveform.value().frameCount()));
    printLine(stdout, bandSummary(waveform.value()).chopped(1));
    printLine(stdout, u"overview   "_s + sparkline(waveform.value(), 72));
    const Result<CacheKey> key = CacheKey::forContent(*source);
    if (key.ok()) {
        printLine(stdout, u"cache key  "_s + key.value().toHex());
    }
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"media-analyze", u"<file>", u"Decode and analyse an audio file: stream facts, 3-band waveform, timing.", &run};

} // namespace
