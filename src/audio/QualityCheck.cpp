// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/QualityCheck.h"

#include "audio/Decoder.h"
#include "audio/detail/Ffmpeg.h"
#include "audio/detail/SpectrumAnalysis.h"
#include "audio/detail/ThreadHandOver.h"

#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::audio {
namespace {

using detail::kSpectrumFftBins;
using detail::kSpectrumFftSize;

constexpr qint64 kChunkFrames = 8192;
constexpr double kMinSeconds = 2.0;
constexpr int kMinWindows = 16;
constexpr int kMinRate = 32000;
constexpr int kMaxSpectrumFrames = 10000;
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

// Per-bin level in dB (relative to a full-scale sine's bin) of a summed power spectrum, averaged over its windows and
// smoothed over kSmoothingHz.
std::vector<double> spectrumLevels(const std::vector<double> &power, int windows, int sampleRate)
{
    const double binHz = static_cast<double>(sampleRate) / kSpectrumFftSize;
    const int half = std::max(1, static_cast<int>(std::lround(kSmoothingHz / binHz / 2.0)));
    std::vector<double> prefix(kSpectrumFftBins + 1, 0.0);
    for (int k = 0; k < kSpectrumFftBins; ++k) {
        prefix[static_cast<std::size_t>(k) + 1] =
            prefix[static_cast<std::size_t>(k)] + power[static_cast<std::size_t>(k)];
    }
    const double fullScale = detail::PowerSpectrum::fullScalePower();
    std::vector<double> db(kSpectrumFftBins, kSilenceDb * 2);
    for (int k = 0; k < kSpectrumFftBins; ++k) {
        const int from = std::max(0, k - half);
        const int to = std::min(kSpectrumFftBins - 1, k + half);
        const double mean = (prefix[static_cast<std::size_t>(to) + 1] - prefix[static_cast<std::size_t>(from)]) /
                            (to - from + 1) / std::max(1, windows);
        db[static_cast<std::size_t>(k)] = mean > 0.0 ? 10.0 * std::log10(mean / fullScale) : kSilenceDb * 2;
    }
    return db;
}

// The loudest smoothed bin between two frequencies.
double peakLevel(const std::vector<double> &db, int sampleRate, double fromHz, double toHz)
{
    const double binHz = static_cast<double>(sampleRate) / kSpectrumFftSize;
    const int from = std::clamp(static_cast<int>(std::ceil(fromHz / binHz)), 0, kSpectrumFftBins - 1);
    const int to = std::clamp(static_cast<int>(std::floor(toHz / binHz)), from, kSpectrumFftBins - 1);
    return *std::max_element(db.begin() + from, db.begin() + to + 1);
}

// Mean level of the bins between two frequencies.
double bandLevel(const std::vector<double> &db, int sampleRate, double fromHz, double toHz)
{
    const double binHz = static_cast<double>(sampleRate) / kSpectrumFftSize;
    const int from = std::clamp(static_cast<int>(std::ceil(fromHz / binHz)), 0, kSpectrumFftBins - 1);
    const int to = std::clamp(static_cast<int>(std::floor(toHz / binHz)), from, kSpectrumFftBins - 1);
    double power = 0.0;
    for (int k = from; k <= to; ++k) {
        power += std::pow(10.0, db[static_cast<std::size_t>(k)] / 10.0);
    }
    return 10.0 * std::log10(power / (to - from + 1));
}

// The highest frequency whose level is within `dropDb` of `referenceDb`: everything above it stays below.
int highestWithin(const std::vector<double> &db, int sampleRate, double referenceDb, double dropDb)
{
    const double binHz = static_cast<double>(sampleRate) / kSpectrumFftSize;
    const double threshold = referenceDb - dropDb;
    for (int k = kSpectrumFftBins - 1; k > 0; --k) {
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

QString kiloHertz(int hz)
{
    return u"%1 kHz"_s.arg(hz / 1000.0, 0, 'f', 1);
}

// The exact word-length check: does every sample of a 20/24-bit or float file sit on the 16-bit grid? A float holds
// 24-bit samples exactly, so the test is exact; 32-bit integers aren't checked (float rounds them). Chunks check their
// parts independently: the file is padded when every part is on the grid and some part isn't silent.
class GridCheck
{
public:
    static bool applies(const StreamInfo &info)
    {
        return info.lossless && (info.floatingPoint || (info.bitsPerSample > 16 && info.bitsPerSample <= 24));
    }

    explicit GridCheck(bool active)
        : m_onGrid(active)
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

    bool onGrid() const { return m_onGrid; }
    bool anyNonZero() const { return m_anyNonZero; }

private:
    bool m_onGrid = false;
    bool m_anyNonZero = false;
};

// The cut-off and, above 48 kHz, the content bandwidth, when the spectrum has enough windows and a reference band.
void measureSpectrum(const std::vector<double> &power, int windows, int sampleRate, QualityReport &report)
{
    if (windows < kMinWindows) {
        return;
    }
    const std::vector<double> db = spectrumLevels(power, windows, sampleRate);
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

// ---------------------------------------------------------------------------------------------------------------
// The decoding pass

// What every chunk of one pass shares.
struct PassSetup
{
    bool gridCheck = false;
    bool spectrogram = false;
    int rows = 0;
    detail::SlotGeometry geometry;
    qint64 slotCap = 0;
};

// One chunk: decodes from `decodeFrom` (a window before the frames it owns, so its first windows have their audio)
// until it has every window it owns and has passed `ownedEnd` (-1: the last chunk, to the end of the stream).
// Chunk boundaries depend only on the length and the chunk count, never on the spectrogram, so the long-term
// spectrum and the grid check come out the same with and without one.
struct ChunkPlan
{
    qint64 decodeFrom = 0;
    qint64 ownedEnd = -1;
    std::vector<qint64> spectrumStarts; // the long-term windows it owns (those starting in its frames)
    qint64 firstSlot = 0;               // the spectrogram slots it owns (those starting in its frames)
    qint64 endSlot = -1;
};

struct ChunkResult
{
    bool onGrid = true;
    bool anyNonZero = false;
    std::vector<double> spectrumSums;
    int spectrumWindows = 0;
    std::vector<detail::SpectrogramSlot> spectrogramSlots;
    std::optional<qint64> endOfStream; // where the stream ended, when this chunk reached the end
    std::optional<Error> error;
};

void runChunk(Decoder &decoder, detail::PowerSpectrum &fft, const PassSetup &setup, const ChunkPlan &plan,
              const std::function<bool()> &stopped, ChunkResult &result)
{
    // A worker's decoder carries on from its previous chunk: move it unless it already stands at this one.
    if (decoder.position() != plan.decodeFrom && decoder.seek(plan.decodeFrom) != DecodeStatus::Ok) {
        result.error = decoder.error();
        return;
    }
    GridCheck grid(setup.gridCheck);
    detail::SpectrumSampler sampler(plan.spectrumStarts);
    std::optional<detail::SpectrogramBuilder> builder;
    if (setup.spectrogram) {
        builder.emplace(setup.rows, setup.geometry, plan.firstSlot, plan.endSlot, setup.slotCap);
    }
    const auto neededFrom = [&sampler, &builder]() -> std::optional<qint64> {
        std::optional<qint64> needed = sampler.neededFrom();
        if (builder) {
            if (const std::optional<qint64> builderNeeds = builder->neededFrom()) {
                needed = needed ? std::min(*needed, *builderNeeds) : builderNeeds;
            }
        }
        return needed;
    };
    const int channels = decoder.channels();
    detail::MonoHistory history;
    std::vector<float> buffer(static_cast<std::size_t>(kChunkFrames * channels));
    qint64 position = plan.decodeFrom;
    while (true) {
        if (stopped()) {
            result.error = makeError(ErrorCode::Cancelled, u"cancelled"_s);
            return;
        }
        std::optional<qint64> needed = neededFrom();
        if (plan.ownedEnd >= 0 && position >= plan.ownedEnd && !needed) {
            break; // the next chunk takes it from here
        }
        const ReadResult read = decoder.read(buffer.data(), kChunkFrames);
        const std::span<const float> samples(buffer.data(), static_cast<std::size_t>(read.frames * channels));
        grid.add(samples);
        // Only the frames some window needs are mixed and kept: without a spectrogram, about 5 % of a long file.
        if (needed) {
            history.appendMix(samples, channels, position, *needed);
            sampler.process(history, fft);
            if (builder) {
                builder->process(history, fft);
            }
            needed = neededFrom();
            history.dropBefore(needed ? *needed : history.end());
        }
        position += read.frames;
        if (read.status == DecodeStatus::EndOfStream) {
            if (builder) {
                builder->finish(history, position, fft);
            }
            result.endOfStream = position;
            break;
        }
        if (read.status == DecodeStatus::Error) {
            result.error = decoder.error();
            return;
        }
        if (read.status == DecodeStatus::NeedMoreData) {
            result.error = makeError(ErrorCode::Internal, u"the decoder wants more of a complete file"_s);
            return;
        }
    }
    result.onGrid = grid.onGrid();
    result.anyNonZero = grid.anyNonZero();
    result.spectrumSums = sampler.sums();
    result.spectrumWindows = sampler.windows();
    if (builder) {
        result.spectrogramSlots = builder->takeSlots();
    }
}

// The chunks of one pass, shared by the calling thread and the pool tasks.
struct PassWork
{
    PassSetup setup;
    std::vector<ChunkPlan> plans;
    std::vector<ChunkResult> results;
    std::atomic<std::size_t> next{0};
    CancellationFlag failed; // set by the first chunk that fails, so the others stop early
    const CancellationFlag *cancel = nullptr;
    std::mutex mutex;
    std::condition_variable allDone;
    std::size_t done = 0;

    // Takes chunks until none is left. Called by the caller and by every pool task; a task that starts after the
    // pass is over finds nothing left and touches nothing else (`cancel` belongs to the caller).
    void work(const Decoder &prototype)
    {
        std::unique_ptr<Decoder> decoder;
        std::unique_ptr<detail::PowerSpectrum> fft;
        const auto stopped = [this] { return failed.isCancelled() || (cancel != nullptr && cancel->isCancelled()); };
        for (std::size_t k = next.fetch_add(1); k < plans.size(); k = next.fetch_add(1)) {
            ChunkResult &result = results[k];
            if (!decoder) {
                Result<std::unique_ptr<Decoder>> clone = prototype.clone(decoderOptions());
                if (clone.ok()) {
                    decoder = clone.takeValue();
                } else {
                    result.error = clone.error();
                }
            }
            if (!fft && !result.error) {
                fft = std::make_unique<detail::PowerSpectrum>();
                if (const Result<void> ready = fft->init(); !ready.ok()) {
                    result.error = ready.error();
                    fft.reset();
                }
            }
            if (!result.error) {
                runChunk(*decoder, *fft, setup, plans[k], stopped, result);
            }
            if (result.error) {
                failed.cancel();
            }
            const std::scoped_lock lock(mutex);
            ++done;
            if (done == plans.size()) {
                allDone.notify_all();
            }
        }
    }

    // The error to report: a real failure rather than the cancellations it caused in other chunks.
    std::optional<Error> firstError() const
    {
        std::optional<Error> cancelled;
        for (const ChunkResult &result : results) {
            if (result.error && result.error->code != ErrorCode::Cancelled) {
                return result.error;
            }
            if (result.error && !cancelled) {
                cancelled = result.error;
            }
        }
        return cancelled;
    }

    static DecoderOptions decoderOptions()
    {
        DecoderOptions options;
        options.channels = 0; // the source layout: the grid check needs the samples as stored
        return options;
    }
};

// Splits [0, length) into `count` chunks (count ≥ 1; one open-ended chunk when the length is unknown).
std::vector<ChunkPlan> planChunks(qint64 length, int count, const std::vector<qint64> &spectrumStarts,
                                  detail::SlotGeometry geometry)
{
    std::vector<ChunkPlan> plans(static_cast<std::size_t>(count));
    const auto beginOf = [length, count](qint64 k) { return (k * length) / count; };
    for (int k = 0; k < count; ++k) {
        ChunkPlan &plan = plans[static_cast<std::size_t>(k)];
        const bool last = k == count - 1;
        const qint64 begin = beginOf(k);
        const qint64 end = last ? -1 : beginOf(k + 1);
        plan.decodeFrom = std::max<qint64>(0, begin - kSpectrumFftSize);
        plan.ownedEnd = end;
        for (const qint64 start : spectrumStarts) {
            if (start >= begin && (last || start < end)) {
                plan.spectrumStarts.push_back(start);
            }
        }
        plan.firstSlot = geometry.firstAtOrAfter(begin);
        plan.endSlot = last ? -1 : geometry.firstAtOrAfter(end);
    }
    return plans;
}

Result<void> checkOptions(const QualityOptions &options)
{
    if (options.spectrumFrames < 1 || options.spectrumFrames > kMaxSpectrumFrames) {
        return makeError(
            ErrorCode::InvalidArgument,
            u"spectrumFrames %1 out of range (1…%2)"_s.arg(options.spectrumFrames).arg(kMaxSpectrumFrames));
    }
    const SpectrogramOptions &grid = options.spectrogramOptions;
    if (options.spectrogram && (grid.columns < 1 || grid.columns > kSpectrogramMaxColumns || grid.bins < 1 ||
                                grid.bins > kSpectrogramMaxBins)) {
        return makeError(ErrorCode::InvalidArgument,
                         u"spectrogram of %1 × %2 out of range (1…%3 × 1…%4)"_s.arg(grid.columns)
                             .arg(grid.bins)
                             .arg(kSpectrogramMaxColumns)
                             .arg(kSpectrogramMaxBins));
    }
    return {};
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

QString findingBadge(QualityFinding finding, std::optional<int> likelySourceKbps)
{
    switch (finding) {
    case QualityFinding::LikelyFromLossy:
        return likelySourceKbps ? u"Likely from a ≈ %1 kbps MP3"_s.arg(*likelySourceKbps)
                                : u"Likely from a lossy file"_s;
    case QualityFinding::LikelyUpconvertedMp3:
        return likelySourceKbps ? u"Likely upconverted from ≈ %1 kbps"_s.arg(*likelySourceKbps)
                                : u"Likely upconverted from a lower bitrate"_s;
    case QualityFinding::LikelyReencoded:
        return u"Likely re-encoded from a lossy file"_s;
    case QualityFinding::LikelyPadded24Bit:
        return u"Likely 16-bit, padded"_s;
    case QualityFinding::LikelyUpsampled:
        return u"Likely upsampled"_s;
    }
    return {};
}

QString findingName(QualityFinding finding)
{
    switch (finding) {
    case QualityFinding::LikelyFromLossy:
        return u"from-lossy"_s;
    case QualityFinding::LikelyUpconvertedMp3:
        return u"upconverted-mp3"_s;
    case QualityFinding::LikelyReencoded:
        return u"reencoded"_s;
    case QualityFinding::LikelyPadded24Bit:
        return u"padded-24-bit"_s;
    case QualityFinding::LikelyUpsampled:
        return u"upsampled"_s;
    }
    return {};
}

std::optional<QualityFinding> findingFromName(QStringView name)
{
    for (const QualityFinding finding :
         {QualityFinding::LikelyFromLossy, QualityFinding::LikelyUpconvertedMp3, QualityFinding::LikelyReencoded,
          QualityFinding::LikelyPadded24Bit, QualityFinding::LikelyUpsampled}) {
        if (name == findingName(finding)) {
            return finding;
        }
    }
    return std::nullopt;
}

Result<QualityReport> analyzeQuality(const std::shared_ptr<ByteSource> &source, const QualityOptions &options,
                                     const CancellationFlag *cancel)
{
    if (!source || !source->snapshot().isComplete()) {
        return makeError(ErrorCode::InvalidArgument, u"the quality checks need a complete file"_s);
    }
    if (const Result<void> valid = checkOptions(options); !valid.ok()) {
        return valid.error();
    }
    detail::installFfmpegLogging();
    Decoder decoder;
    switch (decoder.open(source, PassWork::decoderOptions())) {
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

    const qint64 length = decoder.lengthFrames();
    const bool spectral = info.sampleRate >= kMinRate && length >= static_cast<qint64>(kMinSeconds * info.sampleRate);
    const std::vector<qint64> spectrumStarts =
        spectral ? detail::evenWindowStarts(length, options.spectrumFrames) : std::vector<qint64>{};

    auto work = std::make_shared<PassWork>();
    work->cancel = cancel;
    PassSetup &setup = work->setup;
    setup.gridCheck = GridCheck::applies(info);
    setup.spectrogram = options.spectrogram;
    setup.rows = options.spectrogramOptions.bins;
    // One slot per column over the announced length (never more columns than frames); unknown lengths start with
    // narrow slots that merge as the stream goes on.
    const int columns = options.spectrogramOptions.columns;
    setup.geometry = {length, length > 0 ? std::min<qint64>(columns, length) : 0};
    setup.slotCap = columns;

    const int threads = options.maxThreads > 0 ? options.maxThreads : std::max(1, QThread::idealThreadCount());
    const qint64 minChunk = qint64{std::max(1, options.minChunkSeconds)} * std::max(1, info.sampleRate);
    int chunkCount = 1;
    if (info.exactSeek && length > 0 && threads > 1) {
        chunkCount = static_cast<int>(std::clamp<qint64>(length / minChunk, 1, threads));
    }
    work->plans = planChunks(length, chunkCount, spectrumStarts, setup.geometry);
    work->results.resize(work->plans.size());

    // The prototype is only cloned (a const operation; the frame index of a complete source is immutable), except in
    // a single-chunk pass, which decodes with it directly.
    const auto prototype = std::make_shared<Decoder>(std::move(decoder));
    if (chunkCount == 1) {
        detail::PowerSpectrum fft;
        if (const Result<void> ready = fft.init(); !ready.ok()) {
            return ready.error();
        }
        const auto stopped = [cancel] { return cancel != nullptr && cancel->isCancelled(); };
        runChunk(*prototype, fft, setup, work->plans.front(), stopped, work->results.front());
    } else {
        QThreadPool *pool = options.pool != nullptr ? options.pool : QThreadPool::globalInstance();
        for (int t = 1; t < chunkCount; ++t) {
            detail::startTask(pool, [work, prototype] { work->work(*prototype); });
        }
        work->work(*prototype); // the caller takes chunks too, so a saturated pool can't deadlock it
        std::unique_lock lock(work->mutex);
        work->allDone.wait(lock, [&work] { return work->done == work->plans.size(); });
    }
    if (const std::optional<Error> error = work->firstError()) {
        return *error;
    }

    bool onGrid = true;
    bool anyNonZero = false;
    std::vector<double> spectrum(kSpectrumFftBins, 0.0);
    int windows = 0;
    std::optional<qint64> end;
    std::vector<detail::SpectrogramSlot> allSlots;
    for (ChunkResult &result : work->results) {
        onGrid = onGrid && result.onGrid;
        anyNonZero = anyNonZero || result.anyNonZero;
        for (std::size_t k = 0; k < spectrum.size(); ++k) {
            spectrum[k] += result.spectrumSums[k];
        }
        windows += result.spectrumWindows;
        if (const std::optional<qint64> ended = result.endOfStream) {
            end = end ? std::min(*end, *ended) : *ended;
        }
        std::ranges::move(result.spectrogramSlots, std::back_inserter(allSlots));
    }
    measureSpectrum(spectrum, windows, info.sampleRate, report);
    addFindings(info, setup.gridCheck && onGrid && anyNonZero, report);
    if (options.spectrogram) {
        Result<Spectrogram> spectrogram =
            detail::assembleSpectrogram(info.sampleRate, end.value_or(0), columns, setup.rows, std::move(allSlots));
        if (!spectrogram.ok()) {
            return spectrogram.error();
        }
        report.spectrogram = spectrogram.takeValue();
    }
    return report;
}

} // namespace selecta::audio
