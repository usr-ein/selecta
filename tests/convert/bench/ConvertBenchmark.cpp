// SPDX-License-Identifier: GPL-3.0-or-later
// Conversion speed (C2; sizes the conversion worker pool, CONV-8). Run from a Release build:
//   selecta_convert_bench --benchmark_min_time=2s
// Every case is a whole transcode(): decode, encode, tags, full verification decode, sync and rename. The sources are
// synthetic (tones over noise), encoded at start-up: six minutes of 16-bit 44.1 kHz FLAC, and one minute of 24-bit
// 96 kHz FLAC for the resampling case.
#include "SyntheticAudio.h"
#include "convert/QualityAdvisor.h"
#include "convert/Transcoder.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <benchmark/benchmark.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;

namespace {

Pcm tonesOverNoise(double seconds, int sampleRate, int bits)
{
    Pcm signal = tones({55, 220, 880, 3520, 9000}, seconds, sampleRate, 2, 0.12);
    const Pcm noise = whiteNoise(seconds, 17, sampleRate, 2, 0.15);
    for (std::size_t i = 0; i < signal.samples.size(); ++i) {
        signal.samples[i] += noise.samples[i];
    }
    return quantised(signal, bits);
}

QTemporaryDir &folder()
{
    static QTemporaryDir dir;
    return dir;
}

QString writeSource(const QString &name, const QByteArray &bytes)
{
    QString path = folder().filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

const Pcm &sixMinutes()
{
    static const Pcm pcm = tonesOverNoise(360.0, 44100, 16);
    return pcm;
}

const QString &cdSource()
{
    static const QString path = writeSource(u"six-minutes.flac"_s, makeFlac(sixMinutes()));
    return path;
}

const QString &mp3Source()
{
    static const QString path = writeSource(u"six-minutes.mp3"_s, makeMp3(sixMinutes(), {}));
    return path;
}

const QString &hiResSource()
{
    static const QString path =
        writeSource(u"one-minute-96k.flac"_s, makeFlac(tonesOverNoise(60.0, 96000, 24), {.bits = 24}));
    return path;
}

void convert(benchmark::State &state, OutputFormat format, bool hiRes)
{
    const QString &source = hiRes ? hiResSource() : cdSource();
    if (source.isEmpty()) {
        state.SkipWithError("couldn't write the source");
        return;
    }
    const QString extension = format == OutputFormat::Mp3 ? u"mp3"_s : fileExtension(format);
    int run = 0;
    while (state.KeepRunning()) {
        TranscodeRequest request;
        request.sourcePath = source;
        request.outputPath = folder().filePath(u"out-%1.%2"_s.arg(run++).arg(extension));
        request.settings.format = format;
        request.tags.set(keys::Title, u"Benchmark"_s, Provenance::User);
        request.tags.set(keys::Artist, u"Selecta"_s, Provenance::User);
        const Result<TranscodeReport> result = transcode(request);
        if (!result.ok()) {
            state.SkipWithError(result.error().toString().toStdString());
            return;
        }
        QFile::remove(request.outputPath);
    }
}

// probeSourceQuality() with the quality checks: one full decode, the word-length check and 200 FFTs.
void probe(benchmark::State &state, bool mp3)
{
    const QString &source = mp3 ? mp3Source() : cdSource();
    while (state.KeepRunning()) {
        const Result<SourceQuality> quality = probeSourceQuality(source, /*runChecks=*/true);
        if (!quality.ok() || !quality.value().checks) {
            state.SkipWithError("probe failed");
            return;
        }
        int cutoff = quality.value().checks->cutoffHz;
        benchmark::DoNotOptimize(cutoff);
    }
}

} // namespace

BENCHMARK_CAPTURE(probe, mp3_320_quality_checks, true)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(probe, flac16_quality_checks, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_to_mp3_320, OutputFormat::Mp3, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_to_aiff, OutputFormat::Aiff, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_to_flac, OutputFormat::Flac, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_to_wav, OutputFormat::Wav, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_to_alac, OutputFormat::Alac, false)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(convert, flac16_keep_audio, OutputFormat::KeepAudio, false)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_CAPTURE(convert, flac24_96k_1min_to_mp3_320, OutputFormat::Mp3, true)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
