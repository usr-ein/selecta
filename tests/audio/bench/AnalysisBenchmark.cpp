// SPDX-License-Identifier: GPL-3.0-or-later
// Analysis speed (A2, NFR-2: < 300 ms for a 6-minute track once downloaded; A7: the quality checks with the
// spectrogram, aim < 400 ms). Run from a Release build:
//   selecta_audio_bench --benchmark_min_time=2s
// The test signal is synthetic: six minutes of tones over noise (SELECTA_BENCH_SECONDS overrides 360; the ctest smoke
// run uses a few seconds), encoded on first use.
#include "SyntheticAudio.h"
#include "audio/BandAnalyzer.h"
#include "audio/Decoder.h"
#include "audio/QualityCheck.h"
#include "audio/WaveformAnalyzer.h"

#include <QCoreApplication>

#include <benchmark/benchmark.h>
#include <cstdlib>
#include <memory>
#include <string_view>

using namespace selecta::audio;
using namespace selecta::testing;

namespace {

double benchSeconds()
{
    const char *value = std::getenv("SELECTA_BENCH_SECONDS"); // NOLINT(concurrency-mt-unsafe): read before any thread
    const double seconds = value != nullptr ? std::atof(value) : 360.0; // NOLINT(cert-err34-c): a bench knob
    return seconds >= 1.0 ? seconds : 360.0;
}

const Pcm &sixMinutes()
{
    static const Pcm pcm = [] {
        const double seconds = benchSeconds();
        Pcm signal = tones({55, 220, 880, 3520, 9000}, seconds, 44100, 2, 0.12);
        const Pcm noise = whiteNoise(seconds, 17, 44100, 2, 0.15);
        for (std::size_t i = 0; i < signal.samples.size(); ++i) {
            signal.samples[i] += noise.samples[i];
        }
        return quantised(signal, 16);
    }();
    return pcm;
}

const QByteArray &file(std::string_view format)
{
    if (format == "mp3") {
        static const QByteArray mp3 = makeMp3(sixMinutes(), {});
        return mp3;
    }
    if (format == "flac") {
        static const QByteArray flac = makeFlac(sixMinutes());
        return flac;
    }
    static const QByteArray wav = makeWav(sixMinutes());
    return wav;
}

void decodeOnly(benchmark::State &state, const char *format)
{
    const auto source = std::make_shared<MemorySource>(file(format));
    std::vector<float> buffer(8192);
    while (state.KeepRunning()) {
        Decoder decoder;
        DecoderOptions options;
        options.channels = 1;
        if (decoder.open(source, options) != Decoder::OpenStatus::Ready) {
            state.SkipWithError("open failed");
            return;
        }
        qint64 frames = 0;
        ReadResult r;
        do {
            r = decoder.read(buffer.data(), 8192);
            frames += r.frames;
        } while (r.status == DecodeStatus::Ok);
        benchmark::DoNotOptimize(frames);
    }
}

void analysis(benchmark::State &state, const char *format, int threads)
{
    const auto source = std::make_shared<MemorySource>(file(format));
    AnalysisOptions options;
    options.maxThreads = threads;
    while (state.KeepRunning()) {
        const Result<Waveform> waveform = analyzeWaveform(source, options);
        if (!waveform.ok()) {
            state.SkipWithError("analysis failed");
            return;
        }
        benchmark::DoNotOptimize(waveform.value().detail().size());
    }
}

// The quality checks (A3), with the spectrogram (A7) when asked: one decoding pass, chunked unless threads is 1.
void quality(benchmark::State &state, const char *format, bool spectrogram, int threads)
{
    const auto source = std::make_shared<MemorySource>(file(format));
    QualityOptions options;
    options.spectrogram = spectrogram;
    options.maxThreads = threads;
    while (state.KeepRunning()) {
        const Result<QualityReport> report = analyzeQuality(source, options);
        if (!report.ok() || report.value().spectrogram.has_value() != spectrogram) {
            state.SkipWithError("quality check failed");
            return;
        }
        int cutoff = report.value().cutoffHz;
        benchmark::DoNotOptimize(cutoff);
    }
}

void bandSplit(benchmark::State &state)
{
    std::vector<float> mono(static_cast<std::size_t>(sixMinutes().frames()));
    for (std::size_t i = 0; i < mono.size(); ++i) {
        mono[i] = 0.5F * (sixMinutes().samples[2 * i] + sixMinutes().samples[(2 * i) + 1]);
    }
    while (state.KeepRunning()) {
        BandAnalyzer analyzer(44100);
        QList<WaveformColumn> columns;
        analyzer.process(mono, columns);
        analyzer.finish(columns);
        benchmark::DoNotOptimize(columns.size());
    }
}

} // namespace

BENCHMARK_CAPTURE(decodeOnly, mp3_320, "mp3")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(decodeOnly, flac_16, "flac")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(decodeOnly, wav_16, "wav")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(quality, flac_16_spectrogram_parallel, "flac", true, 0)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(quality, flac_16_parallel, "flac", false, 0)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(quality, flac_16_spectrogram_sequential, "flac", true, 1)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(quality, flac_16_sequential, "flac", false, 1)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(quality, mp3_320_spectrogram_parallel, "mp3", true, 0)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(bandSplit)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(analysis, mp3_320_sequential, "mp3", 1)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(analysis, mp3_320_parallel, "mp3", 0)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(analysis, flac_16_sequential, "flac", 1)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(analysis, flac_16_parallel, "flac", 0)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK_CAPTURE(analysis, wav_16_parallel, "wav", 0)->Unit(benchmark::kMillisecond)->UseRealTime();

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
