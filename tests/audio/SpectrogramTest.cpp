// SPDX-License-Identifier: GPL-3.0-or-later
// A7: the spectrogram built in the quality pass. Dimensions and the level mapping; where a tone and a lowpass edge
// land; silence, short, mono, stereo and 96 kHz files; cancellation and failures; chunked passes equal to sequential
// ones and the checks unchanged by the spectrogram. Then the slot builder on its own, for streams whose length is
// unknown or wrong.
#include "audio/Spectrogram.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/QualityCheck.h"
#include "audio/detail/SpectrumAnalysis.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>

using namespace Qt::StringLiterals;
using namespace selecta::audio;
using namespace selecta::testing;

namespace {

QualityOptions withSpectrogram(int columns = 800, int bins = 256)
{
    QualityOptions options;
    options.spectrogram = true;
    options.spectrogramOptions = {.columns = columns, .bins = bins};
    return options;
}

// Several chunks even for files of a few seconds.
QualityOptions chunked(QualityOptions options, int threads = 4)
{
    options.maxThreads = threads;
    options.minChunkSeconds = 1;
    return options;
}

QualityOptions sequential(QualityOptions options)
{
    options.maxThreads = 1;
    return options;
}

Result<QualityReport> check(const QByteArray &file, const QualityOptions &options,
                            const CancellationFlag *cancel = nullptr)
{
    return analyzeQuality(std::make_shared<MemorySource>(file), options, cancel);
}

QualityReport analyse(const QByteArray &file, const QualityOptions &options)
{
    Result<QualityReport> result = check(file, options);
    if (!result.ok()) {
        ADD_FAILURE() << result.error().toString().toStdString();
        return {};
    }
    return result.takeValue();
}

// The report's spectrogram (a test failure and an empty grid when it has none).
Spectrogram gridOf(const QualityReport &report)
{
    if (!report.spectrogram) {
        ADD_FAILURE() << "no spectrogram";
        return {};
    }
    return *report.spectrogram;
}

QualityReport withoutSpectrogram(QualityReport report)
{
    report.spectrogram.reset();
    return report;
}

// Broadband material: noise plus a few tones, so every band has content up to Nyquist.
Pcm broadband(double seconds, int sampleRate = 44100, int channels = 2)
{
    Pcm signal = tones({110, 1000, 7000}, seconds, sampleRate, channels, 0.08);
    const Pcm noise = whiteNoise(seconds, 99, sampleRate, channels, 0.25);
    for (std::size_t i = 0; i < signal.samples.size(); ++i) {
        signal.samples[i] += noise.samples[i];
    }
    return signal;
}

Pcm decoded(const QByteArray &file, int sampleRate = 0)
{
    DecoderOptions options;
    options.channels = 0;
    options.sampleRate = sampleRate;
    options.resampler = ResamplerQuality::VeryHigh;
    return decodeAll(file, options).pcm;
}

int loudestBin(const Spectrogram &s, int column)
{
    const std::span<const quint8> levels = s.column(column);
    return static_cast<int>(std::ranges::max_element(levels) - levels.begin());
}

// Over every column and the bins [fromBin, toBin].
quint8 highestLevel(const Spectrogram &s, int fromBin, int toBin)
{
    quint8 level = 0;
    for (int c = 0; c < s.columns(); ++c) {
        for (int b = fromBin; b <= toBin; ++b) {
            level = std::max(level, s.level(c, b));
        }
    }
    return level;
}

quint8 lowestLevel(const Spectrogram &s, int fromBin, int toBin)
{
    quint8 level = 255;
    for (int c = 0; c < s.columns(); ++c) {
        for (int b = fromBin; b <= toBin; ++b) {
            level = std::min(level, s.level(c, b));
        }
    }
    return level;
}

// A row's mean level over the columns, in dB: single cells of noise vary by several dB, a row's mean doesn't.
double rowDb(const Spectrogram &s, int bin)
{
    double sum = 0.0;
    for (int c = 0; c < s.columns(); ++c) {
        sum += Spectrogram::dbForLevel(s.level(c, bin));
    }
    return s.columns() > 0 ? sum / s.columns() : kSpectrogramFloorDb;
}

// The rows' mean levels around `bin`, for failure messages.
std::string profile(const Spectrogram &s, int fromBin, int toBin)
{
    std::string text;
    for (int b = std::max(0, fromBin); b <= std::min(s.bins() - 1, toBin); ++b) {
        text += std::to_string(b) + ": " + std::to_string(static_cast<int>(std::lround(rowDb(s, b)))) + " dB  ";
    }
    return text;
}

// A complete in-memory file that can cancel a flag on its n-th read and fail reads reaching past a byte offset.
class ScriptedSource final : public ByteSource
{
public:
    ScriptedSource(QByteArray data, CancellationFlag *cancel, int cancelOnRead, qint64 failFrom)
        : m_data(std::move(data))
        , m_cancel(cancel)
        , m_cancelOnRead(cancelOnRead)
        , m_failFrom(failFrom)
    {}

    qint64 totalSize() const override { return m_data.size(); }
    SourceSnapshot snapshot() const override { return {m_data.size(), SourceState::Complete}; }
    QString description() const override { return u"scripted"_s; }

    qint64 read(qint64 position, char *destination, qint64 maxBytes) override
    {
        const int n = ++m_reads;
        if (m_cancel != nullptr && n == m_cancelOnRead) {
            m_cancel->cancel();
        }
        if (position < 0 || maxBytes < 0) {
            return -1;
        }
        const qint64 count = std::clamp<qint64>(m_data.size() - position, 0, maxBytes);
        if (m_failFrom >= 0 && count > 0 && position + count > m_failFrom) {
            setLastError(makeError(ErrorCode::ReadFailed, u"scripted read failure"_s));
            return -1;
        }
        std::memcpy(destination, m_data.constData() + position, static_cast<std::size_t>(count));
        return count;
    }

    int reads() const { return m_reads.load(); }

private:
    const QByteArray m_data;
    CancellationFlag *m_cancel = nullptr;
    const int m_cancelOnRead = 0;
    const qint64 m_failFrom = -1;
    std::atomic<int> m_reads{0};
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// The data class and the level mapping

TEST(Spectrogram, LevelMappingIsLinearInDb)
{
    EXPECT_EQ(Spectrogram::levelForDb(kSpectrogramFloorDb), 0);
    EXPECT_EQ(Spectrogram::levelForDb(-200.0), 0);
    EXPECT_EQ(Spectrogram::levelForDb(-std::numeric_limits<double>::infinity()), 0);
    EXPECT_EQ(Spectrogram::levelForDb(std::numeric_limits<double>::quiet_NaN()), 0);
    EXPECT_EQ(Spectrogram::levelForDb(kSpectrogramCeilingDb), 255);
    EXPECT_EQ(Spectrogram::levelForDb(12.0), 255);
    EXPECT_EQ(Spectrogram::levelForDb(std::numeric_limits<double>::infinity()), 255);
    EXPECT_EQ(Spectrogram::levelForDb(-60.0), 128); // 127.5 rounds up
    EXPECT_DOUBLE_EQ(Spectrogram::dbForLevel(0), kSpectrogramFloorDb);
    EXPECT_DOUBLE_EQ(Spectrogram::dbForLevel(255), kSpectrogramCeilingDb);
    for (int level = 0; level <= 255; ++level) {
        const auto code = static_cast<quint8>(level);
        EXPECT_EQ(Spectrogram::levelForDb(Spectrogram::dbForLevel(code)), code);
    }
    for (int step = 0; step < 325; ++step) { // -119.9 dB up to just below 0 dB
        const double db = -119.9 + (0.37 * step);
        EXPECT_NEAR(Spectrogram::dbForLevel(Spectrogram::levelForDb(db)), db, (120.0 / 255.0 / 2.0) + 1e-9) << db;
    }
}

TEST(Spectrogram, FromLevelsValidatesTheGrid)
{
    const QByteArray levels(qsizetype{4} * 3, '\x10');
    const Result<Spectrogram> good = Spectrogram::fromLevels(48000, 1000, 4, 3, levels);
    ASSERT_TRUE(good.ok());
    const Spectrogram &s = good.value();
    EXPECT_FALSE(s.isNull());
    EXPECT_FALSE(s.isEmpty());
    EXPECT_EQ(s.columns(), 4);
    EXPECT_EQ(s.bins(), 3);
    EXPECT_DOUBLE_EQ(s.durationSeconds(), 1000.0 / 48000.0);
    EXPECT_DOUBLE_EQ(s.nyquistHz(), 24000.0);
    EXPECT_DOUBLE_EQ(s.binLowHz(1), 8000.0);
    EXPECT_EQ(s.binForFrequency(0.0), 0);
    EXPECT_EQ(s.binForFrequency(7999.0), 0);
    EXPECT_EQ(s.binForFrequency(8000.0), 1);
    EXPECT_EQ(s.binForFrequency(24000.0), 2); // Nyquist is in the top bin
    EXPECT_EQ(s.binForFrequency(1e9), 2);
    EXPECT_EQ(s.binForFrequency(-5.0), 0);
    EXPECT_EQ(s.binForFrequency(std::numeric_limits<double>::quiet_NaN()), 0);
    EXPECT_EQ(s.columnStartFrame(0), 0);
    EXPECT_EQ(s.columnStartFrame(1), 250);
    EXPECT_EQ(s.columnStartFrame(4), 1000);
    EXPECT_EQ(s.columnStartFrame(9), 1000);
    EXPECT_EQ(s.level(3, 2), 0x10);
    EXPECT_EQ(s.level(4, 0), 0); // outside the grid
    EXPECT_EQ(s.level(0, 3), 0);
    EXPECT_EQ(s.level(-1, 0), 0);
    EXPECT_EQ(s.column(1).size(), 3U);
    EXPECT_TRUE(s.column(4).empty());
    EXPECT_TRUE(s.column(-1).empty());

    EXPECT_FALSE(Spectrogram::fromLevels(0, 1000, 4, 3, levels).ok());
    EXPECT_FALSE(Spectrogram::fromLevels(800000, 1000, 4, 3, levels).ok());
    EXPECT_FALSE(Spectrogram::fromLevels(48000, 1000, 4, 0, {}).ok());
    EXPECT_FALSE(Spectrogram::fromLevels(48000, 1000, 4, 4, levels).ok()); // wrong size
    EXPECT_FALSE(Spectrogram::fromLevels(48000, 3, 4, 3, levels).ok());    // more columns than frames
    EXPECT_FALSE(Spectrogram::fromLevels(48000, 1000, -1, 3, {}).ok());
    EXPECT_FALSE(Spectrogram::fromLevels(48000, 100000, kSpectrogramMaxColumns + 1, 1,
                                         QByteArray(kSpectrogramMaxColumns + 1, '\0'))
                     .ok());
    EXPECT_FALSE(
        Spectrogram::fromLevels(48000, 1000, 1, kSpectrogramMaxBins + 1, QByteArray(kSpectrogramMaxBins + 1, '\0'))
            .ok());
    const Result<Spectrogram> empty = Spectrogram::fromLevels(44100, 0, 0, 256, {});
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().isEmpty());
    EXPECT_FALSE(empty.value().isNull());
    EXPECT_TRUE(Spectrogram().isNull());
}

// ---------------------------------------------------------------------------------------------------------------
// The spectrogram of real passes

TEST(Spectrogram, DimensionsFollowTheOptions)
{
    const QByteArray file = makeFlac(quantised(broadband(10.0), 16));
    const QualityReport report = analyse(file, withSpectrogram());
    const Spectrogram s = gridOf(report);
    EXPECT_EQ(s.columns(), 800);
    EXPECT_EQ(s.bins(), 256);
    EXPECT_EQ(s.levels().size(), 800 * 256);
    EXPECT_EQ(s.sampleRate(), 44100);
    EXPECT_EQ(s.frameCount(), 441000);
    EXPECT_DOUBLE_EQ(s.durationSeconds(), 10.0);

    const Spectrogram small = gridOf(analyse(file, withSpectrogram(100, 64)));
    EXPECT_EQ(small.columns(), 100);
    EXPECT_EQ(small.bins(), 64);
    const Spectrogram tall = gridOf(analyse(file, withSpectrogram(1, kSpectrogramMaxBins)));
    EXPECT_EQ(tall.columns(), 1);
    EXPECT_EQ(tall.bins(), kSpectrogramMaxBins);
    const Spectrogram wide = gridOf(analyse(file, withSpectrogram(kSpectrogramMaxColumns, 1)));
    EXPECT_EQ(wide.columns(), kSpectrogramMaxColumns);
    EXPECT_EQ(wide.bins(), 1);

    // Without the option there is none, and nothing else changes.
    const QualityReport plain = analyse(file, {});
    EXPECT_FALSE(plain.spectrogram.has_value());
    EXPECT_EQ(withoutSpectrogram(report), plain);
}

TEST(Spectrogram, LevelsSpanTheRangeWithoutOverflowing)
{
    // A full-scale sine near 0 dBFS (255 when centred on an FFT bin, up to 1.4 dB lower between bins); quantisation
    // noise of 16 bits far below the floor.
    const double binHz = 44100.0 / detail::kSpectrumFftSize;
    const Spectrogram centred = gridOf(analyse(
        makeWav(sine(binHz * 200, 3.0, 44100, 2, 1.0), {.bits = 32, .floatingPoint = true}), withSpectrogram()));
    EXPECT_GE(highestLevel(centred, 0, 255), 254);
    const Spectrogram between = gridOf(analyse(
        makeWav(sine(binHz * 200.5, 3.0, 44100, 2, 1.0), {.bits = 32, .floatingPoint = true}), withSpectrogram()));
    EXPECT_GE(highestLevel(between, 0, 255), Spectrogram::levelForDb(-2.0));
    EXPECT_LE(highestLevel(between, 0, 255), Spectrogram::levelForDb(-1.0));
    // A float file beyond full scale clamps at 255.
    const Spectrogram loud = gridOf(
        analyse(makeWav(sine(1000, 3.0, 44100, 2, 4.0), {.bits = 32, .floatingPoint = true}), withSpectrogram()));
    EXPECT_EQ(highestLevel(loud, 0, 255), 255);
}

TEST(Spectrogram, ASineLandsInItsBin)
{
    const Spectrogram s = gridOf(analyse(makeWav(sine(1000, 4.0)), withSpectrogram()));
    const int bin = static_cast<int>(1000.0 / (22050.0 / 256)); // 11
    ASSERT_EQ(s.binForFrequency(1000.0), bin);
    for (int c = 0; c < s.columns(); ++c) {
        ASSERT_EQ(loudestBin(s, c), bin) << "column " << c;
    }
    // Amplitude 0.5 is -6 dBFS; the bins a few away hold only the window's leakage (-53 dB next door, -110 dB four
    // bins away), the far ones the floor, but for the 16-bit rounding's distortion products, about -115 dB.
    EXPECT_GE(lowestLevel(s, bin, bin), Spectrogram::levelForDb(-7.0));
    EXPECT_LE(highestLevel(s, bin, bin), Spectrogram::levelForDb(-5.5));
    EXPECT_LE(highestLevel(s, bin + 4, 255), Spectrogram::levelForDb(-100.0)) << profile(s, bin - 6, bin + 6);
    EXPECT_LE(highestLevel(s, 0, bin - 4), Spectrogram::levelForDb(-100.0)) << profile(s, bin - 6, bin + 6);
    double far = 0.0;
    for (int b = bin + 40; b < s.bins(); ++b) {
        far += rowDb(s, b) / (s.bins() - bin - 40);
    }
    EXPECT_LT(far, -118.0);
    EXPECT_LE(highestLevel(s, bin + 40, 255), Spectrogram::levelForDb(-108.0));
}

TEST(Spectrogram, ALowpassEdgeStaysSharp)
{
    // Noise with nothing above 16 kHz, like an MP3's lowpass but exact.
    const QByteArray file =
        makeWav(brickwallNoise(16000.0, 5.0, 1234, 44100, 2, 0.1), {.bits = 32, .floatingPoint = true});
    const QualityReport report = analyse(file, chunked(withSpectrogram()));
    const Spectrogram s = gridOf(report);
    const int edge = s.binForFrequency(16000.0);
    ASSERT_EQ(edge, 185);
    const std::string around = profile(s, edge - 3, edge + 6);
    // Below the edge: noise at -20 dBFS RMS, about -47 dB per FFT bin (a few dB more for the largest of a row's
    // eight). Every row carries it, the one holding the edge too: a row takes the largest of its FFT bins, not
    // their mean, so the edge doesn't dim it.
    double inBand = 0.0;
    for (int b = 1; b < edge; ++b) {
        EXPECT_GT(rowDb(s, b), -50.0) << b;
        EXPECT_LT(rowDb(s, b), -38.0) << b;
        inBand += rowDb(s, b) / (edge - 1);
    }
    EXPECT_GT(rowDb(s, edge), inBand - 2.0) << around;
    EXPECT_GE(lowestLevel(s, 1, edge), Spectrogram::levelForDb(-65.0));
    // Above it, only the Hann window's leakage, falling fast: then nothing at all.
    EXPECT_LT(rowDb(s, edge + 1), -70.0) << around;
    EXPECT_LT(rowDb(s, edge + 2), -80.0) << around;
    EXPECT_LE(highestLevel(s, edge + 4, 255), Spectrogram::levelForDb(-110.0)) << around;
    EXPECT_NEAR(report.cutoffHz, 16000, 200);
}

TEST(Spectrogram, A128kMp3MadeLosslessShowsItsCutoff)
{
    // Noise lowpassed by LAME at 128 kbps (lowpass 17 kHz), decoded and stored as FLAC: the classic fake lossless.
    const Pcm from128 = quantised(decoded(makeMp3(quantised(broadband(8.0), 16), {.bitrateKbps = 128})), 16);
    const QByteArray flac = makeFlac(from128);
    const QualityReport report = analyse(flac, chunked(withSpectrogram()));
    const Spectrogram s = gridOf(report);
    ASSERT_TRUE(report.hasFinding(QualityFinding::LikelyFromLossy)) << report.cutoffHz;
    // The rows below the cut-off carry the noise (about -45 dB); LAME's transition band takes about 600 Hz; every row
    // above that sits at the floor.
    const int cutoff = s.binForFrequency(report.cutoffHz);
    const std::string around = profile(s, cutoff - 4, cutoff + 12);
    EXPECT_GT(cutoff, s.binForFrequency(15000.0));
    for (int b = 2; b < s.binForFrequency(15000.0); ++b) {
        EXPECT_GT(rowDb(s, b), -55.0) << b << ": " << around;
    }
    for (int b = s.binForFrequency(report.cutoffHz + 600.0); b < s.bins(); ++b) {
        EXPECT_LT(rowDb(s, b), -110.0) << b << ": " << around;
    }
    EXPECT_LE(highestLevel(s, s.binForFrequency(17500.0), 255), Spectrogram::levelForDb(-100.0)) << around;
    // The checks are the same with and without the spectrogram.
    EXPECT_EQ(withoutSpectrogram(report), analyse(flac, chunked({})));
}

TEST(Spectrogram, ChecksAreUnchangedByTheSpectrogram)
{
    const Pcm cd = quantised(broadband(12.0), 16);
    const Pcm from192 = quantised(decoded(makeMp3(cd, {.bitrateKbps = 192})), 16);
    const std::vector<QByteArray> files = {
        makeWav(cd),
        makeFlac(from192),
        makeMp3(from192, {.bitrateKbps = 320}),
        makeFlac(quantised(broadband(6.0, 96000), 24), {.bits = 24}),
        makeFlac(cd, {.bits = 24}),
    };
    for (const QByteArray &file : files) {
        for (const int threads : {1, 3, 8}) {
            const QualityReport with = analyse(file, chunked(withSpectrogram(), threads));
            const QualityReport without = analyse(file, chunked({}, threads));
            EXPECT_TRUE(with.spectrogram.has_value());
            EXPECT_EQ(withoutSpectrogram(with), without)
                << threads << " threads, cut-off " << with.cutoffHz << " vs " << without.cutoffHz;
        }
    }
}

TEST(Spectrogram, ChunkedPassesMatchSequentialOnes)
{
    const Pcm cd = quantised(broadband(12.0), 16);
    for (const QByteArray &file : {makeFlac(cd), makeWav(cd), makeMp3(cd, {.bitrateKbps = 320})}) {
        const QualityReport one = analyse(file, sequential(withSpectrogram()));
        for (const int threads : {2, 5, 12}) {
            const QualityReport many = analyse(file, chunked(withSpectrogram(), threads));
            // Every column's windows come from one chunk and the decoders are exact after a seek: the same bytes.
            EXPECT_EQ(many.spectrogram, one.spectrogram) << threads;
            // The long-term spectrum adds the chunks' sums in another order: the same within rounding.
            EXPECT_NEAR(many.cutoffHz, one.cutoffHz, 11) << threads;
            EXPECT_EQ(many.findings, one.findings);
        }
    }
}

TEST(Spectrogram, SilenceIsTheFloor)
{
    const QualityReport report = analyse(makeFlac(silence(4.0)), chunked(withSpectrogram()));
    const Spectrogram s = gridOf(report);
    EXPECT_EQ(s.columns(), 800);
    EXPECT_EQ(highestLevel(s, 0, 255), 0);
    EXPECT_EQ(report.cutoffHz, -1);
    EXPECT_TRUE(report.findings.isEmpty());
}

TEST(Spectrogram, ShortFiles)
{
    // Half a second: still the full grid, each column's window centred on it as far as the file allows.
    const Spectrogram half = gridOf(analyse(makeWav(sine(1000, 0.5)), withSpectrogram()));
    EXPECT_EQ(half.columns(), 800);
    EXPECT_EQ(half.frameCount(), 22050);
    for (int c = 0; c < half.columns(); ++c) {
        ASSERT_EQ(loudestBin(half, c), 11) << c;
    }
    EXPECT_GE(lowestLevel(half, 11, 11), Spectrogram::levelForDb(-7.0));

    // 50 ms, shorter than one FFT window: every column is the same zero-padded window, which reads lower.
    const Spectrogram blip = gridOf(analyse(makeWav(sine(1000, 0.05)), withSpectrogram()));
    EXPECT_EQ(blip.columns(), 800);
    for (int c = 1; c < blip.columns(); ++c) {
        ASSERT_TRUE(std::ranges::equal(blip.column(c), blip.column(0))) << c;
    }
    EXPECT_EQ(loudestBin(blip, 0), 11);
    EXPECT_LT(blip.level(0, 11), Spectrogram::levelForDb(-7.0));
    EXPECT_GT(blip.level(0, 11), Spectrogram::levelForDb(-14.0));

    // Fewer frames than columns: one column per frame.
    const Spectrogram tiny = gridOf(analyse(makeWav(sine(1000, 300.0 / 44100)), withSpectrogram()));
    EXPECT_EQ(tiny.columns(), 300);
    EXPECT_EQ(tiny.frameCount(), 300);
    const Spectrogram single = gridOf(analyse(makeWav(sine(1000, 1.0 / 44100)), withSpectrogram()));
    EXPECT_EQ(single.columns(), 1);
}

TEST(Spectrogram, MonoAndStereo)
{
    // The mono mix of a centred signal is the signal: mono and stereo files give the same grid.
    const Spectrogram mono = gridOf(analyse(makeWav(sine(3000, 3.0, 44100, 1)), withSpectrogram()));
    const Spectrogram stereo = gridOf(analyse(makeWav(sine(3000, 3.0, 44100, 2)), withSpectrogram()));
    EXPECT_EQ(mono, stereo);

    // Different signals on each side: both show, each at half its amplitude (-6 dB) in the mix.
    const Spectrogram pair = gridOf(analyse(makeWav(stereoPair(440, 3000, 3.0)), withSpectrogram()));
    const int left = pair.binForFrequency(440);
    const int right = pair.binForFrequency(3000);
    const quint8 expected = Spectrogram::levelForDb(20.0 * std::log10(0.4 / 2.0));
    for (const int bin : {left, right}) {
        EXPECT_GE(lowestLevel(pair, bin, bin), expected - 4) << bin;
        EXPECT_LE(highestLevel(pair, bin, bin), expected + 1) << bin;
    }

    // Surround files are mixed down too.
    const Spectrogram six = gridOf(analyse(makeWav(sine(3000, 3.0, 44100, 6)), withSpectrogram()));
    EXPECT_EQ(six, stereo);
}

TEST(Spectrogram, HighRateFilesReachTheirNyquist)
{
    const Spectrogram genuine =
        gridOf(analyse(makeFlac(quantised(broadband(5.0, 96000), 24), {.bits = 24}), chunked(withSpectrogram())));
    EXPECT_EQ(genuine.sampleRate(), 96000);
    EXPECT_DOUBLE_EQ(genuine.nyquistHz(), 48000.0);
    EXPECT_DOUBLE_EQ(genuine.binLowHz(255), 47812.5);
    EXPECT_EQ(genuine.binForFrequency(48000.0), 255);
    // White noise fills every row up to Nyquist.
    for (int b = 1; b < genuine.bins(); ++b) {
        EXPECT_GT(rowDb(genuine, b), -55.0) << b;
    }

    // Upsampled from 44.1 kHz: nothing above the resampler's filter, around 22 kHz.
    const Pcm upsampled = quantised(decoded(makeWav(quantised(broadband(5.0), 16)), 96000), 24);
    const QualityReport fake = analyse(makeFlac(upsampled, {.bits = 24}), chunked(withSpectrogram()));
    const Spectrogram s = gridOf(fake);
    for (int b = 1; b < s.binForFrequency(20000.0); ++b) {
        EXPECT_GT(rowDb(s, b), -55.0) << b;
    }
    EXPECT_LE(highestLevel(s, s.binForFrequency(23000.0), 255), Spectrogram::levelForDb(-110.0))
        << profile(s, s.binForFrequency(20000.0), s.binForFrequency(25000.0));
    EXPECT_TRUE(fake.hasFinding(QualityFinding::LikelyUpsampled));
}

TEST(Spectrogram, OptionsOutOfRangeAreRefused)
{
    const QByteArray file = makeWav(sine(1000, 1.0));
    for (const auto &[columns, bins] : std::vector<std::pair<int, int>>{
             {0, 256}, {kSpectrogramMaxColumns + 1, 256}, {800, 0}, {800, kSpectrogramMaxBins + 1}, {-1, -1}}) {
        const Result<QualityReport> result = check(file, withSpectrogram(columns, bins));
        ASSERT_FALSE(result.ok()) << columns << " × " << bins;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
    QualityOptions ignored;
    ignored.spectrogramOptions = {.columns = 0, .bins = 0}; // not asked for: not checked
    EXPECT_TRUE(check(file, ignored).ok());
    for (const int frames : {0, -3, 10001}) {
        QualityOptions options;
        options.spectrumFrames = frames;
        const Result<QualityReport> result = check(file, options);
        ASSERT_FALSE(result.ok()) << frames;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(Spectrogram, CancellationStopsEveryChunk)
{
    const QByteArray wav = makeWav(quantised(broadband(12.0), 16));
    for (const QualityOptions &options : {chunked(withSpectrogram()), sequential(withSpectrogram())}) {
        CancellationFlag before;
        before.cancel();
        Result<QualityReport> result = check(wav, options, &before);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error().code, ErrorCode::Cancelled);

        // Cancelled from inside the pass, a few reads into the decoding: every chunk stops within a buffer.
        CancellationFlag during;
        const auto source = std::make_shared<ScriptedSource>(wav, &during, 12, -1);
        result = analyzeQuality(source, options, &during);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
        EXPECT_LT(source->reads(), 12 + 64) << "reads after the cancellation";
    }
}

TEST(Spectrogram, AFailingChunkFailsThePass)
{
    // A read error in the middle of the audio (a WAV opens on its header alone): the error, not the cancellations it
    // causes in the other chunks.
    const QByteArray wav = makeWav(quantised(broadband(12.0), 16));
    for (const QualityOptions &options : {chunked(withSpectrogram()), sequential(withSpectrogram())}) {
        const auto source = std::make_shared<ScriptedSource>(wav, nullptr, 0, wav.size() / 2);
        const Result<QualityReport> result = analyzeQuality(source, options);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error().code, ErrorCode::ReadFailed) << result.error().toString().toStdString();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The slot builder on its own: streams whose length is unknown, shorter or longer than announced

namespace {

using detail::kSpectrumFftSize;
using detail::MonoHistory;
using detail::PowerSpectrum;
using detail::SlotGeometry;
using detail::SpectrogramBuilder;
using detail::SpectrogramSlot;

std::vector<float> monoSine(double frequency, qint64 frames, double amplitude = 0.5)
{
    std::vector<float> samples(static_cast<std::size_t>(frames));
    for (qint64 i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / 44100.0));
    }
    return samples;
}

// Streams `mono` (frames [from, mono.size())) through `builder` in pieces of the given sizes, as the quality pass
// does, and finishes it at the end.
std::vector<SpectrogramSlot> stream(SpectrogramBuilder &builder, const std::vector<float> &mono, qint64 from,
                                    const std::vector<qint64> &pieces)
{
    PowerSpectrum fft;
    if (!fft.init().ok()) {
        ADD_FAILURE() << "no FFT";
        return {};
    }
    MonoHistory history;
    qint64 position = from;
    std::size_t next = 0;
    const auto end = static_cast<qint64>(mono.size());
    while (position < end) {
        const qint64 size = std::min(end - position, pieces.empty() ? 8192 : pieces[next++ % pieces.size()]);
        if (const std::optional<qint64> needed = builder.neededFrom()) {
            history.appendMix(std::span<const float>(mono).subspan(static_cast<std::size_t>(position),
                                                                   static_cast<std::size_t>(size)),
                              1, position, *needed);
            builder.process(history, fft);
            const std::optional<qint64> still = builder.neededFrom();
            history.dropBefore(still ? *still : history.end());
            EXPECT_LE(static_cast<qint64>(history.frames.size()), kSpectrumFftSize + size) << "history bounded";
        }
        position += size;
    }
    builder.finish(history, end, fft);
    return builder.takeSlots();
}

bool sameSlots(const std::vector<SpectrogramSlot> &a, const std::vector<SpectrogramSlot> &b)
{
    return std::ranges::equal(a, b, [](const SpectrogramSlot &x, const SpectrogramSlot &y) {
        return x.start == y.start && x.end == y.end && x.windows == y.windows && x.rows == y.rows;
    });
}

} // namespace

TEST(SpectrogramBuilder, AnExactLengthGivesOneSlotPerColumn)
{
    const qint64 length = 200000;
    const std::vector<float> mono = monoSine(1000, length);
    SpectrogramBuilder builder(64, SlotGeometry{length, 100}, 0, -1, 100);
    const std::vector<SpectrogramSlot> built = stream(builder, mono, 0, {});
    ASSERT_EQ(built.size(), 100U);
    for (std::size_t i = 0; i < built.size(); ++i) {
        EXPECT_EQ(built[i].start, static_cast<qint64>(i) * length / 100);
        EXPECT_EQ(built[i].end, static_cast<qint64>(i + 1) * length / 100);
        EXPECT_EQ(built[i].windows, 1); // 2 000-frame columns: one window each
        EXPECT_EQ(built[i].rows.size(), 64U);
    }
    // Pieces of any size give the same slots, bit for bit.
    SpectrogramBuilder again(64, SlotGeometry{length, 100}, 0, -1, 100);
    EXPECT_TRUE(sameSlots(stream(again, mono, 0, {1, 777, 4096, 13, 30000}), built));

    // Long columns average up to eight windows.
    SpectrogramBuilder wide(64, SlotGeometry{length, 4}, 0, -1, 4);
    const std::vector<SpectrogramSlot> four = stream(wide, mono, 0, {});
    ASSERT_EQ(four.size(), 4U);
    EXPECT_EQ(four[0].windows, 8);
}

TEST(SpectrogramBuilder, ChunksTogetherEqualOnePass)
{
    const qint64 length = 300000;
    const std::vector<float> mono = monoSine(2500, length);
    const SlotGeometry geometry{length, 333};
    SpectrogramBuilder whole(128, geometry, 0, -1, 333);
    const std::vector<SpectrogramSlot> one = stream(whole, mono, 0, {});
    // Three chunks owning the slots that start in their thirds, each decoding from one window before its third.
    std::vector<SpectrogramSlot> joined;
    for (int k = 0; k < 3; ++k) {
        const qint64 begin = (k * length) / 3;
        const qint64 end = ((k + 1) * length) / 3;
        SpectrogramBuilder part(128, geometry, geometry.firstAtOrAfter(begin),
                                k == 2 ? -1 : geometry.firstAtOrAfter(end), 333);
        // A chunk stops once it has its windows; the stream helper runs to the end, which a bounded builder ignores.
        std::ranges::move(stream(part, mono, std::max<qint64>(0, begin - kSpectrumFftSize), {5000}),
                          std::back_inserter(joined));
    }
    EXPECT_TRUE(sameSlots(joined, one));
}

TEST(SpectrogramBuilder, AStreamShorterThanAnnounced)
{
    // Announced 200 000 frames, ended at 120 000 (a truncated file): the columns cover what was decoded.
    const std::vector<float> mono = monoSine(1000, 120000);
    SpectrogramBuilder builder(64, SlotGeometry{200000, 100}, 0, -1, 100);
    std::vector<SpectrogramSlot> built = stream(builder, mono, 0, {});
    ASSERT_EQ(built.size(), 60U);
    EXPECT_EQ(built.back().end, 120000);
    const Result<Spectrogram> s = detail::assembleSpectrogram(44100, 120000, 100, 64, std::move(built));
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().columns(), 60);
    EXPECT_EQ(s.value().frameCount(), 120000);
}

TEST(SpectrogramBuilder, AStreamLongerThanAnnouncedStaysBounded)
{
    // Announced 50 000 frames, ran for 3 000 000 (a lying header): slots merge in pairs, memory stays bounded and the
    // columns still cover the whole stream evenly.
    const std::vector<float> mono = monoSine(1000, 3000000);
    SpectrogramBuilder builder(32, SlotGeometry{50000, 100}, 0, -1, 100);
    std::vector<SpectrogramSlot> built = stream(builder, mono, 0, {});
    EXPECT_LE(built.size(), 200U + 8U);
    EXPECT_GE(built.size(), 100U);
    for (std::size_t i = 1; i < built.size(); ++i) {
        ASSERT_EQ(built[i].start, built[i - 1].end) << "contiguous";
    }
    const Result<Spectrogram> s = detail::assembleSpectrogram(44100, 3000000, 100, 32, std::move(built));
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().columns(), 100);
    const int bin = s.value().binForFrequency(1000.0);
    for (int c = 0; c < 100; ++c) {
        EXPECT_EQ(loudestBin(s.value(), c), bin) << c;
        EXPECT_GE(s.value().level(c, bin), Spectrogram::levelForDb(-7.0)) << c;
    }
}

TEST(SpectrogramBuilder, AnUnknownLength)
{
    // No announced length: narrow slots merging as the stream goes on.
    const std::vector<float> mono = monoSine(1000, 2000000);
    SpectrogramBuilder builder(32, SlotGeometry{}, 0, -1, 100);
    std::vector<SpectrogramSlot> built = stream(builder, mono, 0, {});
    EXPECT_GE(built.size(), 100U);
    EXPECT_LE(built.size(), 200U + 8U);
    const Result<Spectrogram> s = detail::assembleSpectrogram(44100, 2000000, 100, 32, std::move(built));
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().columns(), 100);
    for (int c = 0; c < 100; ++c) {
        EXPECT_EQ(loudestBin(s.value(), c), s.value().binForFrequency(1000.0)) << c;
    }
    // A short stream of unknown length: one 64-frame column per slot.
    SpectrogramBuilder small(32, SlotGeometry{}, 0, -1, 100);
    std::vector<SpectrogramSlot> few = stream(small, monoSine(1000, 3000), 0, {});
    EXPECT_EQ(few.size(), 47U);
    const Result<Spectrogram> brief = detail::assembleSpectrogram(44100, 3000, 100, 32, std::move(few));
    ASSERT_TRUE(brief.ok());
    EXPECT_EQ(brief.value().columns(), 47);
}

TEST(SpectrogramBuilder, NoAudioNoColumns)
{
    SpectrogramBuilder builder(16, SlotGeometry{}, 0, -1, 10);
    const Result<Spectrogram> s = detail::assembleSpectrogram(44100, 0, 10, 16, stream(builder, {}, 0, {}));
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s.value().isEmpty());
    EXPECT_EQ(s.value().bins(), 16);
    // A builder owning no slot (a chunk narrower than a column) is done at once.
    const SpectrogramBuilder none(16, SlotGeometry{1000, 10}, 5, 5, 10);
    EXPECT_FALSE(none.neededFrom().has_value());
}

TEST(SpectrogramBuilder, EvenWindowStarts)
{
    EXPECT_TRUE(detail::evenWindowStarts(kSpectrumFftSize - 1, 10).empty());
    EXPECT_TRUE(detail::evenWindowStarts(100000, 0).empty());
    EXPECT_EQ(detail::evenWindowStarts(kSpectrumFftSize, 10), std::vector<qint64>{0});
    EXPECT_EQ(detail::evenWindowStarts(kSpectrumFftSize + 2, 10), (std::vector<qint64>{0, 1, 2}));
    const std::vector<qint64> starts = detail::evenWindowStarts(100000, 5);
    EXPECT_EQ(starts, (std::vector<qint64>{0, 23976, 47952, 71928, 100000 - kSpectrumFftSize}));
}

TEST(Spectrogram, TruncatedFiles)
{
    // An interrupted download stored as complete: the chunks planned past the real end find nothing and the grid
    // covers what decodes, the same in one pass or many. A FLAC's header announces the full length, so its columns
    // keep their planned span and fewer of them are filled; MP3 and WAV lengths come from what is there.
    const Pcm cd = quantised(broadband(12.0), 16);
    const std::vector<std::pair<QByteArray, int>> files = {
        {makeFlac(cd), 481}, {makeMp3(cd, {.bitrateKbps = 320}), 800}, {makeWav(cd), 800}};
    for (const auto &[whole, columns] : files) {
        const QByteArray cut = whole.left((whole.size() * 6) / 10);
        const Decoded decodedCut = decodeAll(cut, {});
        ASSERT_TRUE(decodedCut.stats.truncated);
        const QualityReport one = analyse(cut, sequential(withSpectrogram()));
        const QualityReport many = analyse(cut, chunked(withSpectrogram()));
        const Spectrogram s = gridOf(one);
        EXPECT_EQ(s.frameCount(), decodedCut.pcm.frames());
        EXPECT_EQ(s.columns(), columns);
        EXPECT_EQ(many.spectrogram, one.spectrogram);
        EXPECT_NEAR(many.cutoffHz, one.cutoffHz, 11);
    }
}
