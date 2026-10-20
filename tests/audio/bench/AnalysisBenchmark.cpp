// SPDX-License-Identifier: GPL-3.0-or-later
// Analysis speed (A2, NFR-2: < 300 ms for a 6-minute track once downloaded). Run from a Release build:
//   selecta_audio_bench --benchmark_min_time=2s
// The test signal is synthetic: six minutes of tones over noise, encoded at start-up.
#include "SyntheticAudio.h"
#include "audio/BandAnalyzer.h"
#include "audio/Decoder.h"
#include "audio/WaveformAnalyzer.h"

#include <QCoreApplication>

#include <benchmark/benchmark.h>
#include <memory>

using namespace selecta::audio;
using namespace selecta::testing;

namespace {

constexpr double kSeconds = 360.0;

const Pcm &sixMinutes()
{
    static const Pcm pcm = [] {
        Pcm signal = tones({55, 220, 880, 3520, 9000}, kSeconds, 44100, 2, 0.12);
        const Pcm noise = whiteNoise(kSeconds, 17, 44100, 2, 0.15);
        for (std::size_t i = 0; i < signal.samples.size(); ++i) {
            signal.samples[i] += noise.samples[i];
        }
        return quantised(signal, 16);
    }();
    return pcm;
}

const QByteArray &file(const char *format)
{
    static const QByteArray mp3 = makeMp3(sixMinutes(), {});
    static const QByteArray flac = makeFlac(sixMinutes());
    static const QByteArray wav = makeWav(sixMinutes());
    if (std::string_view(format) == "mp3") {
        return mp3;
    }
    if (std::string_view(format) == "flac") {
        return flac;
    }
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
