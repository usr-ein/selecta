// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "audio/Spectrogram.h"

#include <QtGlobal>

#include <memory>
#include <optional>
#include <span>
#include <vector>

// The FFT work behind the quality checks and the spectrogram (QualityCheck.cpp): Hann-windowed 4 096-point power
// spectra of the mono mix, taken at planned positions while the decoded audio streams past. Pure computation, no I/O.
// Every class here is used by one thread at a time; the chunked quality pass gives each chunk its own instances.
namespace selecta::audio::detail {

inline constexpr int kSpectrumFftSize = 4096;
inline constexpr int kSpectrumFftBins = (kSpectrumFftSize / 2) + 1;
// The spectrogram averages at most this many FFTs per column; fewer when the column is short (hop ≥ half a window).
inline constexpr int kMaxWindowsPerColumn = 8;
// Column span when a stream's length is unknown: columns start this narrow and merge in pairs as the audio goes on.
inline constexpr qint64 kUnknownLengthSlotFrames = 64;

// Power spectra of Hann-windowed (symmetric) kSpectrumFftSize-point frames, through FFmpeg's av_tx real FFT.
class PowerSpectrum
{
public:
    PowerSpectrum();
    ~PowerSpectrum();
    PowerSpectrum(const PowerSpectrum &) = delete;
    PowerSpectrum &operator=(const PowerSpectrum &) = delete;
    PowerSpectrum(PowerSpectrum &&) = delete;
    PowerSpectrum &operator=(PowerSpectrum &&) = delete;

    // Failure code: Internal (FFmpeg's FFT couldn't start, or out of memory).
    Result<void> init();
    // Adds |X[k]|² of `mono` (at most kSpectrumFftSize frames; missing frames count as zeros) to `sums`
    // (kSpectrumFftBins entries).
    void accumulate(std::span<const float> mono, std::span<double> sums);
    // |X[k]|² of a full-scale sine centred on bin k: the 0 dBFS reference, (N/4)².
    static double fullScalePower();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Mono frames [start, start + frames.size()) of one decoding pass: the history the windows are cut from.
struct MonoHistory
{
    qint64 start = 0;
    std::vector<float> frames;

    qint64 end() const { return start + static_cast<qint64>(frames.size()); }
    // The frames of [from, to) the history holds (empty when none).
    std::span<const float> slice(qint64 from, qint64 to) const;
    // Appends the mono mix ((sum of channels) / channels) of the decoded frames starting at frame `first`, from frame
    // `from` on: earlier ones aren't needed. Buffers must follow each other without gaps.
    void appendMix(std::span<const float> interleaved, int channels, qint64 first, qint64 from);
    // Forgets the frames before `frame` (all of them when `frame` ≥ end()).
    void dropBefore(qint64 frame);
};

// `count` window starts spread evenly over `length` frames (first at 0, last at length − N); none when the length is
// shorter than one window.
std::vector<qint64> evenWindowStarts(qint64 length, int count);

// The long-term spectrum: the summed power of windows at fixed starts. Windows that don't fit before the end of the
// stream are dropped.
class SpectrumSampler
{
public:
    explicit SpectrumSampler(std::vector<qint64> starts);

    // Takes every pending window the history now holds whole.
    void process(const MonoHistory &history, PowerSpectrum &fft);
    // The first frame still needed, or nullopt when every window is done.
    std::optional<qint64> neededFrom() const;

    const std::vector<double> &sums() const { return m_sums; }
    int windows() const { return m_windows; }

private:
    std::vector<qint64> m_starts;
    std::size_t m_next = 0;
    std::vector<double> m_sums;
    int m_windows = 0;
};

// Where the spectrogram's columns ("slots") fall: slot g starts at floor(g · length / count) for a known length, at
// g · kUnknownLengthSlotFrames otherwise. Slots past the planned count continue at the same pace (a stream longer than
// announced).
struct SlotGeometry
{
    qint64 length = 0; // announced length in frames; ≤ 0 when unknown
    qint64 count = 0;  // planned slots for a known length

    bool known() const { return length > 0 && count > 0; }
    qint64 start(qint64 slot) const;
    // The first slot starting at or after `frame` (≥ 0).
    qint64 firstAtOrAfter(qint64 frame) const;
};

// One finished slot: the mean power, per frequency row, of the windows taken in it.
struct SpectrogramSlot
{
    qint64 start = 0;
    qint64 end = 0; // exclusive
    int windows = 0;
    std::vector<float> rows; // per row: the largest mean power of its FFT bins (bin power relative to nothing yet)
};

// Builds the slots [firstSlot, endSlot) of a spectrogram (endSlot < 0: open-ended, until the stream ends) from the
// mono audio of one pass. Each slot averages up to kMaxWindowsPerColumn windows centred evenly in its span (clamped
// into [0, length − N] while the stream is within its announced length), then keeps, per row, the maximum of the
// mean power of the FFT bins in that row. An open-ended builder holding 2 × `slotCap` slots merges them in pairs and
// doubles its span, so memory stays bounded whatever the stream's true length.
class SpectrogramBuilder
{
public:
    SpectrogramBuilder(int rows, SlotGeometry geometry, qint64 firstSlot, qint64 endSlot, qint64 slotCap);

    // Takes every pending window the history now holds whole.
    void process(const MonoHistory &history, PowerSpectrum &fft);
    // The first frame still needed, or nullopt when every slot this builder owns is done.
    std::optional<qint64> neededFrom() const;
    // The stream ended at frame `end` (the history holds what neededFrom() asked for, up to `end`): windows starting
    // before `end` are taken zero-padded, the slot holding `end` is closed there, and later slots are dropped.
    void finish(const MonoHistory &history, qint64 end, PowerSpectrum &fft);

    std::vector<SpectrogramSlot> takeSlots() { return std::move(m_slots); }

private:
    void beginSlot();
    qint64 windowStart(int window) const;
    void closeSlot(qint64 end);
    void mergePairs();

    int m_rows = 0;
    SlotGeometry m_geometry;
    qint64 m_endSlot = -1;
    qint64 m_slotCap = 0;
    std::vector<int> m_rowOfBin;

    qint64 m_slot = 0;     // first fine slot of the current slot
    qint64 m_slotSpan = 1; // fine slots per slot (doubles with every merge)
    qint64 m_slotStart = 0;
    qint64 m_slotEnd = 0;
    int m_slotWindows = 0; // windows planned in the current slot
    int m_nextWindow = 0;
    bool m_done = false;
    std::vector<double> m_sums; // per FFT bin, the current slot's summed power
    int m_summed = 0;
    std::vector<SpectrogramSlot> m_slots;
};

// The finished grid: `allSlots` (every chunk's, in order) over the `frames` decoded frames, reduced to at most
// `columns` columns of `rows` level bytes. With more slots than columns, column c averages the slots whose middle falls
// in its span (the slot under its middle when none does); otherwise each slot is a column. Slots starting at or after
// `frames` are dropped.
Result<Spectrogram> assembleSpectrogram(int sampleRate, qint64 frames, int columns, int rows,
                                        std::vector<SpectrogramSlot> allSlots);

} // namespace selecta::audio::detail
