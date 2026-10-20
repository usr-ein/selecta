// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/QualityCheck.h"

#include "audio/Decoder.h"
#include "audio/detail/Ffmpeg.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::audio {
namespace {

constexpr int kFftSize = 4096;
constexpr int kBins = (kFftSize / 2) + 1;
constexpr qint64 kChunkFrames = 8192;
constexpr double kMinSeconds = 2.0;
constexpr int kMinWindows = 16;
constexpr int kMinRate = 32000;
// The reference band and the level below it that counts as "no content" (calibrated on LAME encodes of noise and
// tones: genuine full-band audio stays within it to Nyquist, LAME's lowpass cuts 60 dB and more).
constexpr double kReferenceLowHz = 14000.0;
constexpr double kReferenceHighHz = 16000.0;
constexpr double kCutoffDropDb = 24.0;
// For files above 48 kHz: anything within this of the reference band is content (genuine high-resolution recordings
// carry harmonics and noise far below the audible band; upsampled ones carry nothing).
constexpr double kContentDropDb = 70.0;
constexpr double kSmoothingHz = 250.0;
constexpr double kSilenceDb = -150.0; // reference band power per bin below this: nothing to measure
// A reference band this far below the loudest part of the spectrum holds only the noise floor (a dark recording, pure
// tones): the cut-off would measure noise, so it isn't measured.
constexpr double kReferenceMinusPeakDb = 80.0;
// Findings (calibrated on LAME 3.100: the measured cut-off lands within about 300 Hz of LAME's lowpass, so 192 kbps
// measures ~18.8 kHz, 160 kbps ~17.5 kHz, 320 kbps ~20.2 kHz, V0 has none).
constexpr int kLosslessCutoffLimitHz = 19200;
constexpr int kUpconvertedCutoffLimitHz = 17800;
constexpr int kUpconvertedMinKbps = 256;
constexpr int kReencodedMarginHz = 1500;
constexpr int kUpsampledLimitHz = 24500;

// LAME's default CBR lowpass by bitrate (libmp3lame/lame.c optimum_bandwidth); a measured cut-off is matched to the
// nearest one within kLowpassMatchHz.
struct LamePoint
{
    int kbps;
    int lowpassHz;
};
constexpr std::array<LamePoint, 6> kLameLowpass = {{
    {128, 17000},
    {160, 17500},
    {192, 18600},
    {224, 19400},
    {256, 19700},
    {320, 20500},
}};
constexpr int kLowpassMatchHz = 800;

struct TxDeleter
{
    void operator()(AVTXContext *context) const { av_tx_uninit(&context); }
};
struct AvFreeDeleter
{
    void operator()(void *pointer) const { av_free(pointer); }
};

// The long-term power spectrum of a mono mix: Hann-windowed FFTs at evenly spaced positions.
class SpectrumAccumulator
{
public:
    Result<void> init()
    {
        AVTXContext *context = nullptr;
        const float scale = 1.0F;
        if (av_tx_init(&context, &m_fn, AV_TX_FLOAT_RDFT, 0, kFftSize, &scale, 0) < 0 || context == nullptr) {
            return makeError(ErrorCode::Internal, u"FFmpeg's FFT couldn't start"_s);
        }
        m_tx.reset(context);
        m_in.reset(static_cast<float *>(av_malloc(sizeof(float) * kFftSize)));
        m_out.reset(static_cast<AVComplexFloat *>(av_malloc(sizeof(AVComplexFloat) * kBins)));
        if (!m_in || !m_out) {
            return makeError(ErrorCode::Internal, u"out of memory"_s);
        }
        m_window.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) {
            m_window[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * i / (kFftSize - 1))));
        }
        m_power.assign(kBins, 0.0);
        return {};
    }

    void add(const float *mono)
    {
        for (int i = 0; i < kFftSize; ++i) {
            m_in.get()[i] = mono[i] * m_window[static_cast<std::size_t>(i)];
        }
        m_fn(m_tx.get(), m_out.get(), m_in.get(), sizeof(float));
        for (int k = 0; k < kBins; ++k) {
            const AVComplexFloat value = m_out.get()[k];
            m_power[static_cast<std::size_t>(k)] += (double{value.re} * value.re) + (double{value.im} * value.im);
        }
        ++m_windows;
    }

    int windows() const { return m_windows; }

    // Per-bin level in dB (relative to a full-scale sine's bin), averaged over the windows and smoothed over
    // kSmoothingHz.
    std::vector<double> levels(int sampleRate) const
    {
        const double binHz = static_cast<double>(sampleRate) / kFftSize;
        const int half = std::max(1, static_cast<int>(std::lround(kSmoothingHz / binHz / 2.0)));
        std::vector<double> prefix(kBins + 1, 0.0);
        for (int k = 0; k < kBins; ++k) {
            prefix[static_cast<std::size_t>(k) + 1] =
                prefix[static_cast<std::size_t>(k)] + m_power[static_cast<std::size_t>(k)];
        }
        // A full-scale sine through a Hann window peaks at (N/4)² per bin.
        const double fullScale = (kFftSize / 4.0) * (kFftSize / 4.0);
        std::vector<double> db(kBins, kSilenceDb * 2);
        for (int k = 0; k < kBins; ++k) {
            const int from = std::max(0, k - half);
            const int to = std::min(kBins - 1, k + half);
            const double mean = (prefix[static_cast<std::size_t>(to) + 1] - prefix[static_cast<std::size_t>(from)]) /
                                (to - from + 1) / std::max(1, m_windows);
            db[static_cast<std::size_t>(k)] = mean > 0.0 ? 10.0 * std::log10(mean / fullScale) : kSilenceDb * 2;
        }
        return db;
    }

private:
    std::unique_ptr<AVTXContext, TxDeleter> m_tx;
    av_tx_fn m_fn = nullptr;
    std::unique_ptr<float, AvFreeDeleter> m_in;
    std::unique_ptr<AVComplexFloat, AvFreeDeleter> m_out;
    std::vector<float> m_window;
    std::vector<double> m_power;
    int m_windows = 0;
};

// The loudest smoothed bin between two frequencies.
double peakLevel(const std::vector<double> &db, int sampleRate, double fromHz, double toHz)
{
    const double binHz = static_cast<double>(sampleRate) / kFftSize;
    const int from = std::clamp(static_cast<int>(std::ceil(fromHz / binHz)), 0, kBins - 1);
    const int to = std::clamp(static_cast<int>(std::floor(toHz / binHz)), from, kBins - 1);
    return *std::max_element(db.begin() + from, db.begin() + to + 1);
}

// Mean level of the bins between two frequencies.
double bandLevel(const std::vector<double> &db, int sampleRate, double fromHz, double toHz)
{
    const double binHz = static_cast<double>(sampleRate) / kFftSize;
    const int from = std::clamp(static_cast<int>(std::ceil(fromHz / binHz)), 0, kBins - 1);
    const int to = std::clamp(static_cast<int>(std::floor(toHz / binHz)), from, kBins - 1);
    double power = 0.0;
    for (int k = from; k <= to; ++k) {
        power += std::pow(10.0, db[static_cast<std::size_t>(k)] / 10.0);
    }
    return 10.0 * std::log10(power / (to - from + 1));
}

// The highest frequency whose level is within `dropDb` of `referenceDb`: everything above it stays below.
int highestWithin(const std::vector<double> &db, int sampleRate, double referenceDb, double dropDb)
{
    const double binHz = static_cast<double>(sampleRate) / kFftSize;
    const double threshold = referenceDb - dropDb;
    for (int k = kBins - 1; k > 0; --k) {
        if (db[static_cast<std::size_t>(k)] >= threshold) {
            return static_cast<int>(std::lround(k * binHz));
        }
    }
    return 0;
}

std::optional<int> likelyKbps(int cutoffHz)
{
    const auto *const nearest = std::ranges::min_element(
        kLameLowpass, {}, [cutoffHz](LamePoint point) { return std::abs(point.lowpassHz - cutoffHz); });
    if (std::abs(nearest->lowpassHz - cutoffHz) > kLowpassMatchHz) {
        return std::nullopt;
    }
    return nearest->kbps;
}

// Evenly spaced window starts over `length` frames.
std::vector<qint64> windowPositions(qint64 length, int count)
{
    std::vector<qint64> positions;
    if (length < kFftSize || count <= 0) {
        return positions;
    }
    const int windows = static_cast<int>(std::min<qint64>(count, (length - kFftSize) + 1));
    positions.reserve(static_cast<std::size_t>(windows));
    for (int i = 0; i < windows; ++i) {
        positions.push_back(windows == 1 ? 0 : (i * (length - kFftSize)) / (windows - 1));
    }
    return positions;
}

QString kiloHertz(int hz)
{
    return u"%1 kHz"_s.arg(hz / 1000.0, 0, 'f', 1);
}

// The exact word-length check: does every sample of a 20/24-bit or float file sit on the 16-bit grid? A float holds
// 24-bit samples exactly, so the test is exact; 32-bit integers aren't checked (float rounds them).
class GridCheck
{
public:
    explicit GridCheck(const StreamInfo &info)
        : m_active(info.lossless && (info.floatingPoint || (info.bitsPerSample > 16 && info.bitsPerSample <= 24)))
        , m_onGrid(m_active)
    {}

    void add(std::span<const float> samples)
    {
        if (!m_onGrid && m_anyNonZero) {
            return; // decided
        }
        for (const float sample : samples) {
            const double scaled = static_cast<double>(sample) * 32768.0;
            m_anyNonZero = m_anyNonZero || sample != 0.0F;
            m_onGrid = m_onGrid && scaled == std::floor(scaled);
        }
    }

    // Padded: every sample on the grid, and not all silence (which proves nothing).
    bool padded() const { return m_active && m_onGrid && m_anyNonZero; }

private:
    bool m_active = false;
    bool m_onGrid = false;
    bool m_anyNonZero = false;
};

// Feeds the spectrum the windows at evenly spaced positions as the decoded audio streams past, keeping only the mono
// history the next window needs.
class SpectrumSampler
{
public:
    SpectrumSampler(SpectrumAccumulator &spectrum, std::vector<qint64> positions)
        : m_spectrum(spectrum)
        , m_positions(std::move(positions))
    {}

    void add(std::span<const float> interleaved, int channels)
    {
        const auto frames = static_cast<qint64>(interleaved.size() / static_cast<std::size_t>(channels));
        const qint64 chunkStart = m_seen;
        m_seen += frames;
        if (m_next >= m_positions.size()) {
            return;
        }
        // Frames before the next window are never needed: only the windows' frames (about 5 % of a long file) are
        // mixed and kept.
        qint64 from = 0;
        if (m_history.empty()) {
            const qint64 needed = m_positions[m_next];
            if (chunkStart + frames <= needed) {
                return;
            }
            from = std::max<qint64>(0, needed - chunkStart);
            m_historyStart = chunkStart + from;
        }
        const auto stride = static_cast<std::size_t>(channels);
        for (qint64 f = from; f < frames; ++f) {
            float sum = 0.0F;
            for (std::size_t c = 0; c < stride; ++c) {
                sum += interleaved[(static_cast<std::size_t>(f) * stride) + c];
            }
            m_history.push_back(sum / static_cast<float>(channels));
        }
        const qint64 historyEnd = m_historyStart + static_cast<qint64>(m_history.size());
        while (m_next < m_positions.size() && m_positions[m_next] + kFftSize <= historyEnd) {
            m_spectrum.add(m_history.data() + (m_positions[m_next] - m_historyStart));
            ++m_next;
        }
        const qint64 keepFrom = m_next < m_positions.size() ? std::min(m_positions[m_next], historyEnd) : historyEnd;
        m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(keepFrom - m_historyStart));
        m_historyStart = keepFrom;
    }

private:
    SpectrumAccumulator &m_spectrum;
    std::vector<qint64> m_positions;
    std::size_t m_next = 0;
    std::vector<float> m_history; // mono, from frame m_historyStart
    qint64 m_historyStart = 0;
    qint64 m_seen = 0; // frames received so far
};

// The cut-off and, above 48 kHz, the content bandwidth, when the spectrum has enough windows and a reference band.
void measureSpectrum(const SpectrumAccumulator &spectrum, int sampleRate, QualityReport &report)
{
    if (spectrum.windows() < kMinWindows) {
        return;
    }
    const std::vector<double> db = spectrum.levels(sampleRate);
    const double reference = bandLevel(db, sampleRate, kReferenceLowHz, kReferenceHighHz);
    const double peak = peakLevel(db, sampleRate, 20.0, kReferenceLowHz);
    if (reference <= kSilenceDb || reference < peak - kReferenceMinusPeakDb) {
        return;
    }
    report.cutoffHz = highestWithin(db, sampleRate, reference, kCutoffDropDb);
    if (sampleRate > 48000) {
        report.contentBandwidthHz = highestWithin(db, sampleRate, reference, kContentDropDb);
    }
}

void addFindings(const StreamInfo &info, bool padded, QualityReport &report)
{
    if (padded) {
        report.effectiveBitDepth = 16;
        report.findings.append(QualityFinding::LikelyPadded24Bit);
    }
    // Spectral findings need a band above 20 kHz to judge (44.1 kHz and up).
    const bool judgeable = report.cutoffHz > 0 && info.sampleRate >= 44100;
    // Above 48 kHz, a genuine recording may roll off steeply in the audible band yet carry ultrasonic content; only a
    // file with nothing above the CD band is judged on its audible cut-off.
    if (judgeable && info.lossless && report.cutoffHz <= kLosslessCutoffLimitHz &&
        (info.sampleRate <= 48000 || report.contentBandwidthHz <= kUpsampledLimitHz)) {
        report.likelySourceKbps = likelyKbps(report.cutoffHz);
        report.findings.append(QualityFinding::LikelyFromLossy);
    }
    const bool mp3 = judgeable && info.codec == Codec::Mp3;
    if (mp3 && info.bitrateKbps >= kUpconvertedMinKbps && report.cutoffHz <= kUpconvertedCutoffLimitHz) {
        report.likelySourceKbps = likelyKbps(report.cutoffHz);
        report.findings.append(QualityFinding::LikelyUpconvertedMp3);
    }
    // A LAME tag at or above Nyquist (V0 records 22.1 kHz at 44.1 kHz) means no lowpass was applied: nothing to
    // compare.
    if (mp3 && report.lameLowpassHz && *report.lameLowpassHz < (info.sampleRate / 2) - 500 &&
        report.cutoffHz + kReencodedMarginHz < *report.lameLowpassHz) {
        if (!report.likelySourceKbps) {
            report.likelySourceKbps = likelyKbps(report.cutoffHz);
        }
        report.findings.append(QualityFinding::LikelyReencoded);
    }
    if (info.sampleRate > 48000 && report.contentBandwidthHz > 0 && report.contentBandwidthHz <= kUpsampledLimitHz) {
        report.findings.append(QualityFinding::LikelyUpsampled);
    }
}

} // namespace

QString describeFinding(QualityFinding finding, const QualityReport &report)
{
    switch (finding) {
    case QualityFinding::LikelyFromLossy:
        return report.likelySourceKbps
                   ? u"Likely from a lossy source (≈ %1 kbps MP3; the spectrum stops at %2)"_s
                         .arg(*report.likelySourceKbps)
                         .arg(kiloHertz(report.cutoffHz))
                   : u"Likely from a lossy source (the spectrum stops at %1)"_s.arg(kiloHertz(report.cutoffHz));
    case QualityFinding::LikelyUpconvertedMp3:
        return report.likelySourceKbps
                   ? u"Likely upconverted from ≈ %1 kbps (the spectrum stops at %2)"_s.arg(*report.likelySourceKbps)
                         .arg(kiloHertz(report.cutoffHz))
                   : u"Likely upconverted from a lower bitrate (the spectrum stops at %1)"_s.arg(
                         kiloHertz(report.cutoffHz));
    case QualityFinding::LikelyReencoded:
        return u"Likely re-encoded from a lossy source (its LAME tag says %1, the spectrum stops at %2)"_s.arg(
            kiloHertz(report.lameLowpassHz.value_or(0)), kiloHertz(report.cutoffHz));
    case QualityFinding::LikelyPadded24Bit:
        return u"Likely %1-bit audio padded to %2 bits"_s.arg(report.effectiveBitDepth).arg(report.sourceBitDepth);
    case QualityFinding::LikelyUpsampled:
        return u"Likely upsampled (no content above %1 in a %2 file)"_s.arg(kiloHertz(report.contentBandwidthHz),
                                                                            kiloHertz(report.sampleRate));
    }
    return {};
}

Result<QualityReport> analyzeQuality(const std::shared_ptr<ByteSource> &source, QualityOptions options,
                                     const CancellationFlag *cancel)
{
    if (!source || !source->snapshot().isComplete()) {
        return makeError(ErrorCode::InvalidArgument, u"the quality checks need a complete file"_s);
    }
    detail::installFfmpegLogging();
    DecoderOptions decoderOptions;
    decoderOptions.channels = 0;
    Decoder decoder;
    switch (decoder.open(source, decoderOptions)) {
    case Decoder::OpenStatus::Ready:
        break;
    case Decoder::OpenStatus::Failed:
        return decoder.error();
    case Decoder::OpenStatus::NeedMoreData:
    case Decoder::OpenStatus::NeedsCompleteFile:
        return makeError(ErrorCode::Internal, u"the decoder wants more of a complete file"_s);
    }
    const StreamInfo &info = decoder.info();
    QualityReport report;
    report.sampleRate = info.sampleRate;
    report.sourceBitDepth = info.lossless ? info.bitsPerSample : 0;
    report.effectiveBitDepth = report.sourceBitDepth;
    if (info.lameTag && info.lameTag->lowpassHz > 0) {
        report.lameLowpassHz = info.lameTag->lowpassHz;
    }

    SpectrumAccumulator spectrum;
    if (const Result<void> ready = spectrum.init(); !ready.ok()) {
        return ready.error();
    }
    const int channels = decoder.channels();
    const qint64 length = decoder.lengthFrames();
    const bool spectral = info.sampleRate >= kMinRate && length >= static_cast<qint64>(kMinSeconds * info.sampleRate);
    SpectrumSampler sampler(spectrum, spectral ? windowPositions(length, std::max(1, options.spectrumFrames))
                                               : std::vector<qint64>{});
    GridCheck grid(info);
    std::vector<float> buffer(static_cast<std::size_t>(kChunkFrames * channels));
    while (true) {
        if (cancel != nullptr && cancel->isCancelled()) {
            return makeError(ErrorCode::Cancelled, u"cancelled"_s);
        }
        const ReadResult read = decoder.read(buffer.data(), kChunkFrames);
        const std::span<const float> samples(buffer.data(), static_cast<std::size_t>(read.frames * channels));
        grid.add(samples);
        sampler.add(samples, channels);
        if (read.status == DecodeStatus::EndOfStream) {
            break;
        }
        if (read.status == DecodeStatus::Error) {
            return decoder.error();
        }
        if (read.status == DecodeStatus::NeedMoreData) {
            return makeError(ErrorCode::Internal, u"the decoder wants more of a complete file"_s);
        }
    }
    measureSpectrum(spectrum, info.sampleRate, report);
    addFindings(info, grid.padded(), report);
    return report;
}

} // namespace selecta::audio
