// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/SpectrumAnalysis.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr qint64 kFftSize = kSpectrumFftSize;
constexpr std::size_t kBins = kSpectrumFftBins;

struct TxDeleter
{
    void operator()(AVTXContext *context) const { av_tx_uninit(&context); }
};
struct AvFreeDeleter
{
    void operator()(void *pointer) const { av_free(pointer); }
};

// Windows in a slot of `span` frames: a hop of at least half a window, at most kMaxWindowsPerColumn.
int windowsForSpan(qint64 span)
{
    const qint64 wanted = ((2 * span) + kFftSize - 1) / kFftSize;
    return static_cast<int>(std::clamp<qint64>(wanted, 1, kMaxWindowsPerColumn));
}

SpectrogramSlot combined(const SpectrogramSlot &a, const SpectrogramSlot &b)
{
    SpectrogramSlot out;
    out.start = a.start;
    out.end = b.end;
    out.windows = a.windows + b.windows;
    out.rows.resize(a.rows.size());
    const double total = out.windows;
    for (std::size_t r = 0; r < out.rows.size(); ++r) {
        out.rows[r] = static_cast<float>(((double{a.rows[r]} * a.windows) + (double{b.rows[r]} * b.windows)) / total);
    }
    return out;
}

quint8 levelForPower(double power)
{
    if (!(power > 0.0)) {
        return 0; // silence, and NaN from a hostile float file
    }
    return Spectrogram::levelForDb(10.0 * std::log10(power / PowerSpectrum::fullScalePower()));
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// PowerSpectrum

struct PowerSpectrum::Impl
{
    std::unique_ptr<AVTXContext, TxDeleter> tx;
    av_tx_fn fn = nullptr;
    std::unique_ptr<float, AvFreeDeleter> in;
    std::unique_ptr<AVComplexFloat, AvFreeDeleter> out;
    std::vector<float> window;
};

PowerSpectrum::PowerSpectrum()
    : m_impl(std::make_unique<Impl>())
{}

PowerSpectrum::~PowerSpectrum() = default;

Result<void> PowerSpectrum::init()
{
    AVTXContext *context = nullptr;
    const float scale = 1.0F;
    if (av_tx_init(&context, &m_impl->fn, AV_TX_FLOAT_RDFT, 0, kFftSize, &scale, 0) < 0 || context == nullptr) {
        return makeError(ErrorCode::Internal, u"FFmpeg's FFT couldn't start"_s);
    }
    m_impl->tx.reset(context);
    m_impl->in.reset(static_cast<float *>(av_malloc(sizeof(float) * kFftSize)));
    m_impl->out.reset(static_cast<AVComplexFloat *>(av_malloc(sizeof(AVComplexFloat) * kBins)));
    if (!m_impl->in || !m_impl->out) {
        return makeError(ErrorCode::Internal, u"out of memory"_s);
    }
    m_impl->window.resize(kFftSize);
    for (qint64 i = 0; i < kFftSize; ++i) {
        m_impl->window[static_cast<std::size_t>(i)] = static_cast<float>(
            0.5 -
            (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(kFftSize - 1))));
    }
    return {};
}

void PowerSpectrum::accumulate(std::span<const float> mono, std::span<double> sums)
{
    Q_ASSERT(m_impl->tx && sums.size() == kBins);
    float *in = m_impl->in.get();
    const std::size_t frames = std::min(mono.size(), static_cast<std::size_t>(kFftSize));
    for (std::size_t i = 0; i < frames; ++i) {
        in[i] = mono[i] * m_impl->window[i];
    }
    std::fill(in + frames, in + kFftSize, 0.0F);
    m_impl->fn(m_impl->tx.get(), m_impl->out.get(), in, sizeof(float));
    const AVComplexFloat *out = m_impl->out.get();
    for (std::size_t k = 0; k < kBins; ++k) {
        sums[k] += (double{out[k].re} * out[k].re) + (double{out[k].im} * out[k].im);
    }
}

double PowerSpectrum::fullScalePower()
{
    constexpr double amplitude = kFftSize / 4.0;
    return amplitude * amplitude;
}

// ---------------------------------------------------------------------------------------------------------------
// MonoHistory

std::span<const float> MonoHistory::slice(qint64 from, qint64 to) const
{
    from = std::max(from, start);
    to = std::min(to, end());
    if (from >= to) {
        return {};
    }
    return {frames.data() + (from - start), static_cast<std::size_t>(to - from)};
}

void MonoHistory::appendMix(std::span<const float> interleaved, int channels, qint64 first, qint64 from)
{
    if (channels <= 0) {
        return;
    }
    if (frames.empty()) {
        start = std::max(first, from);
    }
    const auto stride = static_cast<std::size_t>(channels);
    const auto count = static_cast<qint64>(interleaved.size() / stride);
    // Decoding is contiguous, so the history ends where this buffer starts, or (empty) at the first frame needed.
    for (qint64 f = std::max<qint64>(0, end() - first); f < count; ++f) {
        float sum = 0.0F;
        for (std::size_t c = 0; c < stride; ++c) {
            sum += interleaved[(static_cast<std::size_t>(f) * stride) + c];
        }
        frames.push_back(sum / static_cast<float>(channels));
    }
}

void MonoHistory::dropBefore(qint64 frame)
{
    if (frame <= start) {
        return;
    }
    if (frame >= end()) {
        frames.clear();
        start = frame;
        return;
    }
    frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(frame - start));
    start = frame;
}

std::vector<qint64> evenWindowStarts(qint64 length, int count)
{
    std::vector<qint64> starts;
    if (length < kFftSize || count <= 0) {
        return starts;
    }
    const int windows = static_cast<int>(std::min<qint64>(count, (length - kFftSize) + 1));
    starts.reserve(static_cast<std::size_t>(windows));
    for (int i = 0; i < windows; ++i) {
        starts.push_back(windows == 1 ? 0 : (i * (length - kFftSize)) / (windows - 1));
    }
    return starts;
}

// ---------------------------------------------------------------------------------------------------------------
// SpectrumSampler

SpectrumSampler::SpectrumSampler(std::vector<qint64> starts)
    : m_starts(std::move(starts))
    , m_sums(kBins, 0.0)
{}

void SpectrumSampler::process(const MonoHistory &history, PowerSpectrum &fft)
{
    while (m_next < m_starts.size() && m_starts[m_next] + kFftSize <= history.end()) {
        const qint64 start = m_starts[m_next++];
        Q_ASSERT(start >= history.start); // the pass keeps everything from neededFrom()
        if (start >= history.start) {
            fft.accumulate(history.slice(start, start + kFftSize), m_sums);
            ++m_windows;
        }
    }
}

std::optional<qint64> SpectrumSampler::neededFrom() const
{
    if (m_next >= m_starts.size()) {
        return std::nullopt;
    }
    return m_starts[m_next];
}

// ---------------------------------------------------------------------------------------------------------------
// SlotGeometry

qint64 SlotGeometry::start(qint64 slot) const
{
    return known() ? (slot * length) / slots : slot * kUnknownLengthSlotFrames;
}

qint64 SlotGeometry::firstAtOrAfter(qint64 frame) const
{
    if (frame <= 0) {
        return 0;
    }
    // The smallest g with floor(g · length / slots) ≥ frame is ceil(frame · slots / length).
    return known() ? ((frame * slots) + length - 1) / length
                   : (frame + kUnknownLengthSlotFrames - 1) / kUnknownLengthSlotFrames;
}

// ---------------------------------------------------------------------------------------------------------------
// SpectrogramBuilder

SpectrogramBuilder::SpectrogramBuilder(int rows, SlotGeometry geometry, qint64 firstSlot, qint64 endSlot,
                                       qint64 slotCap)
    : m_rows(std::max(1, rows))
    , m_geometry(geometry)
    , m_endSlot(endSlot)
    , m_slotCap(std::max<qint64>(1, slotCap))
    , m_rowOfBin(kBins)
    , m_slot(std::max<qint64>(0, firstSlot))
    , m_sums(kBins, 0.0)
{
    // FFT bin k (centre k · rate / N) belongs to row floor(k · rows / (N / 2)); the Nyquist bin joins the top row.
    for (std::size_t k = 0; k < kBins; ++k) {
        m_rowOfBin[k] =
            static_cast<int>(std::min<qint64>(m_rows - 1, (static_cast<qint64>(k) * m_rows) / (kFftSize / 2)));
    }
    beginSlot();
}

void SpectrogramBuilder::beginSlot()
{
    if (m_endSlot >= 0 && m_slot >= m_endSlot) {
        m_done = true;
        return;
    }
    m_slotStart = m_geometry.start(m_slot);
    m_slotEnd = m_geometry.start(m_slot + m_slotSpan);
    m_slotWindows = windowsForSpan(m_slotEnd - m_slotStart);
    m_nextWindow = 0;
    m_summed = 0;
    std::ranges::fill(m_sums, 0.0);
}

qint64 SpectrogramBuilder::windowStart(int window) const
{
    const qint64 span = m_slotEnd - m_slotStart;
    const qint64 centre = m_slotStart + (((2 * qint64{window}) + 1) * span) / (2 * qint64{m_slotWindows});
    qint64 start = centre - (kFftSize / 2);
    // Within the announced length, whole windows of audio: the last ones end at the end instead of running past it.
    if (m_geometry.known() && m_slotStart < m_geometry.length) {
        start = std::min(start, std::max<qint64>(0, m_geometry.length - kFftSize));
    }
    return std::max<qint64>(0, start);
}

std::optional<qint64> SpectrogramBuilder::neededFrom() const
{
    if (m_done) {
        return std::nullopt;
    }
    return windowStart(m_nextWindow);
}

void SpectrogramBuilder::closeSlot(qint64 end)
{
    if (m_summed == 0) {
        return;
    }
    SpectrogramSlot slot;
    slot.start = m_slotStart;
    slot.end = std::max(end, m_slotStart);
    slot.windows = m_summed;
    slot.rows.assign(static_cast<std::size_t>(m_rows), 0.0F);
    // The largest mean power among each row's FFT bins: a lowpass edge stays in the row it falls in.
    for (std::size_t k = 0; k < kBins; ++k) {
        float &row = slot.rows[static_cast<std::size_t>(m_rowOfBin[k])];
        row = std::max(row, static_cast<float>(m_sums[k] / m_summed));
    }
    m_slots.push_back(std::move(slot));
}

void SpectrogramBuilder::mergePairs()
{
    std::vector<SpectrogramSlot> merged;
    merged.reserve((m_slots.size() + 1) / 2);
    for (std::size_t i = 0; i + 1 < m_slots.size(); i += 2) {
        merged.push_back(combined(m_slots[i], m_slots[i + 1]));
    }
    if (m_slots.size() % 2 != 0) {
        merged.push_back(std::move(m_slots.back()));
    }
    m_slots = std::move(merged);
    m_slotSpan *= 2;
}

void SpectrogramBuilder::process(const MonoHistory &history, PowerSpectrum &fft)
{
    while (!m_done) {
        const qint64 start = windowStart(m_nextWindow);
        if (start + kFftSize > history.end()) {
            return;
        }
        Q_ASSERT(start >= history.start); // the pass keeps everything from neededFrom()
        fft.accumulate(history.slice(start, start + kFftSize), m_sums);
        ++m_summed;
        if (++m_nextWindow < m_slotWindows) {
            continue;
        }
        closeSlot(m_slotEnd);
        m_slot += m_slotSpan;
        // An open-ended builder past twice its cap: pairs merge, so the stream may go on for any length.
        if (m_endSlot < 0 && std::cmp_greater_equal(m_slots.size(), 2 * m_slotCap)) {
            mergePairs();
        }
        beginSlot();
    }
}

void SpectrogramBuilder::finish(const MonoHistory &history, qint64 end, PowerSpectrum &fft)
{
    while (!m_done && m_slotStart < end) {
        const qint64 start = windowStart(m_nextWindow);
        if (start >= end) {
            break;
        }
        fft.accumulate(history.slice(start, std::min(end, start + kFftSize)), m_sums);
        ++m_summed;
        if (++m_nextWindow < m_slotWindows) {
            continue;
        }
        closeSlot(std::min(m_slotEnd, end));
        m_slot += m_slotSpan;
        beginSlot();
    }
    if (!m_done && m_slotStart < end) {
        closeSlot(std::min(m_slotEnd, end)); // a partial slot, kept when it got at least one window
    }
    m_done = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Assembly

Result<Spectrogram> assembleSpectrogram(int sampleRate, qint64 frames, int columns, int rows,
                                        std::vector<SpectrogramSlot> slots)
{
    const bool rowsMatch = std::ranges::all_of(
        slots, [rows](const SpectrogramSlot &slot) { return std::cmp_equal(slot.rows.size(), rows); });
    if (rows < 1 || rows > kSpectrogramMaxBins || !rowsMatch) {
        return makeError(ErrorCode::Internal, u"spectrogram slots with %1 rows don't fit"_s.arg(rows));
    }
    std::erase_if(slots, [frames](const SpectrogramSlot &slot) { return slot.start >= frames || slot.windows <= 0; });
    if (!slots.empty()) {
        slots.back().end = frames; // the last column runs to the end of the audio
    }
    const auto available = static_cast<qint64>(slots.size());
    const int out = static_cast<int>(std::min<qint64>(std::max(0, columns), available));
    QByteArray levels(qsizetype{out} * rows, '\0');
    const auto put = [&levels, rows](int column, const auto &power) {
        for (int r = 0; r < rows; ++r) {
            levels[(qsizetype{column} * rows) + r] =
                static_cast<char>(levelForPower(power[static_cast<std::size_t>(r)]));
        }
    };
    if (available <= out) {
        for (int c = 0; c < out; ++c) {
            put(c, slots[static_cast<std::size_t>(c)].rows);
        }
    } else {
        // Column c spans [c · frames / out, (c + 1) · frames / out): the window-weighted mean of the slots whose
        // middle falls in it, or the slot under its middle when none does (slots wider than columns).
        std::vector<double> sum(static_cast<std::size_t>(rows));
        std::size_t next = 0;
        std::size_t under = 0;
        const auto middle = [](const SpectrogramSlot &slot) { return slot.start + ((slot.end - slot.start) / 2); };
        for (int c = 0; c < out; ++c) {
            const qint64 from = (qint64{c} * frames) / out;
            const qint64 to = ((qint64{c} + 1) * frames) / out;
            std::ranges::fill(sum, 0.0);
            qint64 windows = 0;
            while (next < slots.size() && middle(slots[next]) < to) {
                const SpectrogramSlot &slot = slots[next++];
                for (std::size_t r = 0; r < sum.size(); ++r) {
                    sum[r] += double{slot.rows[r]} * slot.windows;
                }
                windows += slot.windows;
            }
            if (windows > 0) {
                for (double &value : sum) {
                    value /= static_cast<double>(windows);
                }
                put(c, sum);
                continue;
            }
            const qint64 centre = from + ((to - from) / 2);
            while (under + 1 < slots.size() && slots[under + 1].start <= centre) {
                ++under;
            }
            put(c, slots[under].rows);
        }
    }
    return Spectrogram::fromLevels(sampleRate, frames, out, rows, std::move(levels));
}

} // namespace selecta::audio::detail
