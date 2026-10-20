// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMetaType>
#include <QtGlobal>

#include <array>
#include <span>

// The 3-band waveform (ADR-007, references/audio-preview-and-waveform.md §7): data only. Drawing is the app's job.
namespace selecta::audio {

// Linkwitz–Riley 4th-order crossovers. Named constants because they are to be calibrated against rekordbox 7
// (audio §12.1); changing them changes kAnalysisVersion.
inline constexpr double kLowMidCrossoverHz = 200.0;
inline constexpr double kMidHighCrossoverHz = 2500.0;
// Detail resolution: one column per 1/150 s, Pioneer's PWV7 rate.
inline constexpr int kColumnsPerSecond = 150;
// Long-term overview: a fixed 1 200 columns of 3 peak bytes (3.6 KB), like Pioneer's PWV6.
inline constexpr int kOverviewColumns = 1200;
// Version of the analysis itself (filters, crossovers, column rate, companding). Cached results from another version
// are recomputed.
inline constexpr int kAnalysisVersion = 1;

inline constexpr int kBandCount = 3;
enum class Band { Low, Mid, High };
constexpr int bandIndex(Band band)
{
    return static_cast<int>(band);
}

// One detail column: peak (max |x|) and RMS of each band over 1/150 s of mono audio, √-companded to 0…255 (see
// compandAmplitude). Index with bandIndex().
struct WaveformColumn
{
    std::array<quint8, kBandCount> peak{};
    std::array<quint8, kBandCount> rms{};

    friend bool operator==(const WaveformColumn &, const WaveformColumn &) = default;
};

// One overview column: the largest detail peak of each band in its span.
struct OverviewColumn
{
    std::array<quint8, kBandCount> peak{};

    friend bool operator==(const OverviewColumn &, const OverviewColumn &) = default;
};

} // namespace selecta::audio

// Plain bytes: QList may move them with memcpy.
Q_DECLARE_TYPEINFO(selecta::audio::WaveformColumn, Q_PRIMITIVE_TYPE);
Q_DECLARE_TYPEINFO(selecta::audio::OverviewColumn, Q_PRIMITIVE_TYPE);

namespace selecta::audio {

// q = round(255 · √min(1, a)) for a linear amplitude a ≥ 0 (negative and NaN give 0). √ companding gives quiet passages
// more code values than a linear byte would; the display curve (dB mapping, per-track gain) is applied when drawing.
quint8 compandAmplitude(float amplitude);
// Inverse: the linear amplitude a code stands for, (q / 255)².
float expandAmplitude(quint8 code);

// Column c covers source frames [columnStartFrame(c), columnStartFrame(c + 1)): floor(c · sampleRate / 150), exact
// integer arithmetic, so chunked and progressive analyses agree on every boundary.
qint64 columnStartFrame(qint64 column, int sampleRate);
// Columns needed to cover `frames` source frames (the last one may be partial).
qint64 columnCountForFrames(qint64 frames, int sampleRate);

// A finished (or, during a download, partial) analysis. Immutable and implicitly shared: cheap to copy and to pass
// through queued signals. Thread-safe for concurrent reads.
class Waveform
{
public:
    Waveform() = default;

    // From detail columns; derives the overview. `complete` is false for a partial result (a preview still
    // downloading): the columns then cover only the frames analysed so far.
    static Waveform fromDetail(int sampleRate, qint64 frameCount, QList<WaveformColumn> detail, bool complete = true);
    // Overview only: what the long-term cache tier keeps once the detail has been evicted.
    static Waveform fromOverview(int sampleRate, qint64 frameCount, QList<OverviewColumn> overview);

    bool isNull() const { return m_sampleRate == 0; }
    int sampleRate() const { return m_sampleRate; }
    // Source frames covered (all of them when complete()).
    qint64 frameCount() const { return m_frameCount; }
    bool isComplete() const { return m_complete; }
    double durationSeconds() const;

    bool hasDetail() const { return !m_detail.isEmpty(); }
    const QList<WaveformColumn> &detail() const { return m_detail; }
    // Always kOverviewColumns entries for a non-null waveform with at least one column.
    const QList<OverviewColumn> &overview() const { return m_overview; }

    friend bool operator==(const Waveform &, const Waveform &) = default;

private:
    int m_sampleRate = 0;
    qint64 m_frameCount = 0;
    bool m_complete = false;
    QList<WaveformColumn> m_detail;
    QList<OverviewColumn> m_overview;
};

// The 1 200-column overview of a detail array: column j spans detail columns [j·n/1200, (j+1)·n/1200), at least one;
// each band keeps its largest peak (max of peaks keeps transients visible, audio §7.2). Empty for empty input.
QList<OverviewColumn> deriveOverview(std::span<const WaveformColumn> detail);

// Reduces `columns` to `count` buckets for drawing at another zoom (mip levels, an overview at the real pixel width):
// max of peaks, mean of RMS (in the companded domain, rounded). Bucket i spans [i·n/count, (i+1)·n/count), at least
// one column. Empty when count ≤ 0 or the input is empty.
QList<WaveformColumn> reduceColumns(std::span<const WaveformColumn> columns, int count);

} // namespace selecta::audio

Q_DECLARE_METATYPE(selecta::audio::Waveform)
Q_DECLARE_METATYPE(selecta::audio::WaveformColumn)
