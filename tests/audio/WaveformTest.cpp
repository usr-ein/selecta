// SPDX-License-Identifier: GPL-3.0-or-later
// 3-band analysis (A2): companding, column arithmetic, band separation of tones, silence, clipping, determinism, and
// chunked parallel analysis matching the sequential one exactly.
#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/BandAnalyzer.h"
#include "audio/WaveformAnalyzer.h"

#include <cmath>
#include <gtest/gtest.h>

using namespace selecta::audio;
using namespace selecta::testing;

namespace {

constexpr std::size_t kLow = 0;
constexpr std::size_t kMid = 1;
constexpr std::size_t kHigh = 2;

QList<WaveformColumn> analyse(const Pcm &mono)
{
    BandAnalyzer analyzer(mono.sampleRate);
    QList<WaveformColumn> columns;
    analyzer.process(std::span<const float>(mono.samples), columns);
    analyzer.finish(columns);
    return columns;
}

// Median over the steady-state columns (the first and last 10 columns hold the filters' transients).
std::array<int, kBandCount> steadyPeaks(const QList<WaveformColumn> &columns)
{
    std::array<int, kBandCount> result{};
    for (std::size_t b = 0; b < kBandCount; ++b) {
        // QList, not std::vector<int>: GoogleTest (built without ASan) instantiates std::vector<int> too, and mixing
        // instrumented and uninstrumented copies of its inline functions trips ASan in GoogleTest's own code.
        QList<int> values;
        for (qsizetype i = 10; i < columns.size() - 10; ++i) {
            values.append(columns[i].peak.at(b));
        }
        std::ranges::nth_element(values, values.begin() + (values.size() / 2));
        result.at(b) = values[values.size() / 2];
    }
    return result;
}

} // namespace

TEST(Companding, CurveAndEdges)
{
    EXPECT_EQ(compandAmplitude(0.0F), 0);
    EXPECT_EQ(compandAmplitude(-0.5F), 0);
    EXPECT_EQ(compandAmplitude(std::nanf("")), 0);
    EXPECT_EQ(compandAmplitude(1.0F), 255);
    EXPECT_EQ(compandAmplitude(7.0F), 255);  // clipped input never overflows
    EXPECT_EQ(compandAmplitude(0.25F), 128); // √0.25 = 0.5
    // Quiet passages keep resolution: -48 dBFS still gets a code above zero.
    EXPECT_GT(compandAmplitude(0.004F), 10);
    for (int q = 0; q <= 255; ++q) {
        EXPECT_EQ(compandAmplitude(expandAmplitude(static_cast<quint8>(q))), q);
    }
}

TEST(Columns, BoundariesAreExactIntegers)
{
    EXPECT_EQ(columnStartFrame(1, 44100), 294);
    EXPECT_EQ(columnStartFrame(150, 44100), 44100);
    EXPECT_EQ(columnStartFrame(1, 48000), 320);
    EXPECT_EQ(columnStartFrame(1, 32000), 213); // floor(213.33)
    EXPECT_EQ(columnStartFrame(3, 32000), 640);
    EXPECT_EQ(columnCountForFrames(0, 44100), 0);
    EXPECT_EQ(columnCountForFrames(294, 44100), 1);
    EXPECT_EQ(columnCountForFrames(295, 44100), 2);
    EXPECT_EQ(columnCountForFrames(qint64{44100} * 360, 44100), 54000); // six minutes
    for (const int rate : {8000, 22050, 32000, 44100, 48000, 88200, 96000, 192000}) {
        for (const qint64 frames : {1, 100, 12345, 999999}) {
            const qint64 columns = columnCountForFrames(frames, rate);
            EXPECT_GE(columnStartFrame(columns, rate), frames);
            EXPECT_LT(columnStartFrame(columns - 1, rate), frames);
        }
    }
}

TEST(BandAnalyzer, TonesLandInTheirBands)
{
    struct Case
    {
        double frequency;
        std::size_t band;
    };
    for (const Case &c : {Case{60, kLow}, Case{1000, kMid}, Case{8000, kHigh}}) {
        const std::array<int, kBandCount> peaks = steadyPeaks(analyse(sine(c.frequency, 2.0, 44100, 1, 0.5)));
        // 0.5 → √0.5 · 255 ≈ 180 in its band; LR4 slopes keep the other bands far below.
        EXPECT_GE(peaks.at(c.band), 170) << c.frequency;
        for (std::size_t other = 0; other < kBandCount; ++other) {
            if (other != c.band) {
                EXPECT_LE(peaks.at(other), 45) << c.frequency << " Hz leaks into band " << other;
            }
        }
    }
}

TEST(BandAnalyzer, RmsBelowPeakForSines)
{
    const QList<WaveformColumn> columns = analyse(sine(1000, 1.0, 48000, 1, 0.5));
    for (qsizetype i = 10; i < columns.size() - 10; ++i) {
        // RMS of a sine is peak/√2: one quarter-power step below in the √ domain (×0.84).
        EXPECT_LT(columns[i].rms.at(kMid), columns[i].peak.at(kMid));
        EXPECT_NEAR(columns[i].rms.at(kMid), columns[i].peak.at(kMid) * 0.8409, 3.0);
    }
}

TEST(BandAnalyzer, SilenceIsZero)
{
    const QList<WaveformColumn> columns = analyse(silence(1.0, 44100, 1));
    for (const WaveformColumn &column : columns) {
        EXPECT_EQ(column, WaveformColumn{});
    }
}

TEST(BandAnalyzer, ClippingStaysInRange)
{
    Pcm loud = sine(60, 1.0, 44100, 1, 1.0);
    for (float &s : loud.samples) {
        s = s >= 0 ? 1.5F : -1.5F; // over full scale, as float files can be
    }
    const QList<WaveformColumn> columns = analyse(loud);
    ASSERT_FALSE(columns.isEmpty());
    EXPECT_EQ(steadyPeaks(columns).at(kLow), 255);
}

TEST(BandAnalyzer, ColumnCountAndPartialLastColumn)
{
    const Pcm pcm = sine(440, 1.001, 44100, 1); // 44144 frames: 150 full columns + one partial
    const QList<WaveformColumn> columns = analyse(pcm);
    EXPECT_EQ(columns.size(), columnCountForFrames(pcm.frames(), 44100));
    EXPECT_EQ(columns.size(), 151);
}

TEST(BandAnalyzer, SplittingTheInputChangesNothing)
{
    const Pcm pcm = tones({60, 1000, 8000}, 2.0, 44100, 1);
    const QList<WaveformColumn> whole = analyse(pcm);
    BandAnalyzer pieces(44100);
    QList<WaveformColumn> columns;
    for (std::size_t start = 0; start < pcm.samples.size(); start += 777) {
        const std::size_t count = std::min<std::size_t>(777, pcm.samples.size() - start);
        pieces.process(std::span<const float>(pcm.samples).subspan(start, count), columns);
    }
    pieces.finish(columns);
    EXPECT_EQ(columns, whole);
}

TEST(Overview, DerivedFromDetailByMaxOfPeaks)
{
    QList<WaveformColumn> detail(5000);
    detail[2501].peak = {200, 10, 20};
    const QList<OverviewColumn> overview = deriveOverview(detail);
    ASSERT_EQ(overview.size(), kOverviewColumns);
    int nonZero = 0;
    for (const OverviewColumn &column : overview) {
        if (column.peak[0] != 0) {
            ++nonZero;
            EXPECT_EQ(column.peak[0], 200);
        }
    }
    EXPECT_EQ(nonZero, 1); // the transient survives, exactly once
    EXPECT_EQ(deriveOverview({}).size(), 0);
    // Shorter than the overview: every overview column still reflects a detail column.
    const QList<WaveformColumn> shortDetail(7, WaveformColumn{{9, 9, 9}, {1, 1, 1}});
    const QList<OverviewColumn> shortOverview = deriveOverview(shortDetail);
    for (const OverviewColumn &column : shortOverview) {
        EXPECT_EQ(column.peak[1], 9);
    }
}

TEST(Overview, ReduceColumnsKeepsPeaksAndAveragesRms)
{
    const QList<WaveformColumn> columns = {
        WaveformColumn{{10, 0, 0}, {10, 0, 0}}, WaveformColumn{{50, 0, 0}, {20, 0, 0}},
        WaveformColumn{{30, 0, 0}, {30, 0, 0}}, WaveformColumn{{0, 0, 0}, {0, 0, 0}}};
    const QList<WaveformColumn> reduced = reduceColumns(columns, 2);
    ASSERT_EQ(reduced.size(), 2);
    EXPECT_EQ(reduced[0].peak[0], 50);
    EXPECT_EQ(reduced[0].rms[0], 15);
    EXPECT_EQ(reduced[1].peak[0], 30);
    EXPECT_EQ(reduced[1].rms[0], 15);
    EXPECT_TRUE(reduceColumns(columns, 0).isEmpty());
    EXPECT_EQ(reduceColumns(columns, 10).size(), 10); // upsampling repeats columns
}

TEST(WaveformAnalysis, ChunkedParallelEqualsSequential)
{
    const Pcm source =
        quantised(concatenated(tones({55, 440, 6000}, 12.0, 44100, 2), whiteNoise(10.0, 3, 44100, 2)), 16);
    for (const QByteArray &file : {makeWav(source), makeFlac(source), makeMp3(source, {})}) {
        const auto data = std::make_shared<MemorySource>(file);
        AnalysisOptions sequential;
        sequential.maxThreads = 1;
        AnalysisOptions parallel;
        parallel.maxThreads = 6;
        parallel.minChunkSeconds = 2;
        const Result<Waveform> a = analyzeWaveform(data, sequential);
        const Result<Waveform> b = analyzeWaveform(data, parallel);
        ASSERT_TRUE(a.ok()) << a.error().toString().toStdString();
        ASSERT_TRUE(b.ok()) << b.error().toString().toStdString();
        EXPECT_TRUE(a.value().isComplete());
        EXPECT_EQ(a.value().detail().size(), columnCountForFrames(a.value().frameCount(), 44100));
        EXPECT_EQ(a.value().frameCount(), b.value().frameCount());
        ASSERT_EQ(a.value().detail().size(), b.value().detail().size());
        EXPECT_EQ(a.value(), b.value()); // byte-identical columns
        // Deterministic run to run.
        EXPECT_EQ(analyzeWaveform(data, parallel).value(), b.value());
    }
}

TEST(WaveformAnalysis, ProgressReportsAndCancellation)
{
    const auto data = std::make_shared<MemorySource>(makeWav(sine(440, 30.0)));
    std::atomic<qint64> lastProgress{0};
    AnalysisOptions options;
    options.minChunkSeconds = 5;
    const Result<Waveform> done = analyzeWaveform(data, options, nullptr, [&](qint64 analysed, qint64 total) {
        EXPECT_LE(analysed, total);
        lastProgress = analysed;
    });
    ASSERT_TRUE(done.ok());
    EXPECT_EQ(lastProgress.load(), done.value().frameCount());

    CancellationFlag cancel;
    cancel.cancel();
    const Result<Waveform> cancelled = analyzeWaveform(data, options, &cancel);
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}

TEST(WaveformAnalysis, RefusesIncompleteSourcesAndGarbage)
{
    const QByteArray wav = makeWav(sine(440, 2.0));
    EXPECT_EQ(analyzeWaveform(std::make_shared<MemorySource>(wav, wav.size() / 2)).error().code,
              ErrorCode::NeedsCompleteFile);
    EXPECT_EQ(analyzeWaveform(std::make_shared<MemorySource>(randomBytes(400000, 8))).error().code,
              ErrorCode::Unsupported);
    EXPECT_EQ(analyzeWaveform(nullptr).error().code, ErrorCode::InvalidArgument);
}

TEST(ProgressiveAnalysis, GrowsToTheCompleteResult)
{
    const Pcm source = quantised(tones({60, 1000, 8000}, 8.0, 44100, 2), 16);
    for (const QByteArray &file : {makeFlac(source), makeMp3(source, {}), makeWav(source)}) {
        const Result<Waveform> reference = analyzeWaveform(std::make_shared<MemorySource>(file));
        ASSERT_TRUE(reference.ok());
        const auto growing = std::make_shared<LoggingSource>(file, 0);
        ProgressiveAnalyzer analyzer(growing);
        QList<WaveformColumn> collected;
        ProgressiveAnalyzer::Status status = ProgressiveAnalyzer::Status::NeedMoreData;
        qint64 available = 0;
        for (int guard = 0; guard < 100000 && status != ProgressiveAnalyzer::Status::Finished; ++guard) {
            const ProgressiveAnalyzer::Step step = analyzer.step(44100);
            ASSERT_NE(step.status, ProgressiveAnalyzer::Status::Failed) << analyzer.error().toString().toStdString();
            EXPECT_EQ(step.firstColumn, collected.size());
            collected.append(step.columns);
            status = step.status;
            if (status == ProgressiveAnalyzer::Status::NeedMoreData) {
                available = std::min<qint64>(file.size(), available + 20011);
                growing->setAvailable(available);
                if (available == file.size()) {
                    growing->setComplete();
                }
            }
        }
        ASSERT_EQ(status, ProgressiveAnalyzer::Status::Finished);
        EXPECT_EQ(growing->violations(), 0);
        EXPECT_EQ(collected, reference.value().detail());
        EXPECT_EQ(analyzer.result(), reference.value());
        EXPECT_EQ(analyzer.expectedColumns(), reference.value().detail().size());
    }
}

TEST(ProgressiveAnalysis, WaitsForCompleteFileWhenItMust)
{
    const QByteArray m4a = makeM4a(sine(440, 2.0), {});
    const auto growing = std::make_shared<MemorySource>(m4a, m4a.size() - 10);
    ProgressiveAnalyzer analyzer(growing);
    EXPECT_EQ(analyzer.step().status, ProgressiveAnalyzer::Status::NeedsCompleteFile);
    growing->setComplete();
    const ProgressiveAnalyzer::Step step = analyzer.step();
    EXPECT_EQ(step.status, ProgressiveAnalyzer::Status::Finished);
    EXPECT_FALSE(step.columns.isEmpty());
}
