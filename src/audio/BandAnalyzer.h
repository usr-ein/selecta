// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Waveform.h"

#include <QList>
#include <QtGlobal>

#include <array>
#include <span>

namespace selecta::audio {

// Streaming 3-band analysis of mono audio (ADR-007): Linkwitz–Riley 4th-order crossovers at kLowMidCrossoverHz and
// kMidHighCrossoverHz (each LR4 filter is two cascaded Butterworth biquads, eight biquads in all), then the peak and
// RMS of each band over every 1/150 s column, √-companded.
//
// Filters run in double precision and start settled on silence. Columns are aligned to absolute source frames (see
// columnStartFrame), so analyses of different chunks of one file can be concatenated: start each chunk's analyzer at
// a column boundary and warm its filters up on the audio just before it. 80 ms of warm-up makes a chunk's columns
// identical to a sequential analysis (the filters' memory decays far below the 8-bit output resolution).
//
// Threading: not thread-safe; one instance per thread or per chunk. Pure computation, no allocation in process()
// beyond appending to the output list.
class BandAnalyzer
{
public:
    // `sampleRate` in Hz (8 000…768 000; outside that range it is clamped). The first sample passed to process() is
    // the first frame of column `firstColumn`.
    explicit BandAnalyzer(int sampleRate, qint64 firstColumn = 0);

    // Recommended warm-up before a chunk boundary, in frames (80 ms).
    static qint64 warmUpFrames(int sampleRate);

    // Runs samples through the filters only, producing no columns. Call before the first process().
    void warmUp(std::span<const float> samples);
    // Analyses samples; appends a column to `out` each time one is complete.
    void process(std::span<const float> samples, QList<WaveformColumn> &out);
    // Appends the last column if it is partial (end of stream). Further process() calls start a new column.
    void finish(QList<WaveformColumn> &out);

    int sampleRate() const { return m_sampleRate; }
    // The column the next sample belongs to.
    qint64 currentColumn() const { return m_column; }
    // Frames analysed by process(), not counting warm-up.
    qint64 framesProcessed() const { return m_frames; }

private:
    struct Biquad
    {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // normalised so that a0 = 1
        double z1 = 0, z2 = 0;                         // transposed direct form II state
    };
    struct Accumulator
    {
        std::array<double, kBandCount> peak{};
        std::array<double, kBandCount> sumSquares{};
        qint64 count = 0;
    };

    void filter(float sample, std::array<double, kBandCount> &bands);
    void emitColumn(QList<WaveformColumn> &out);

    int m_sampleRate = 0;
    qint64 m_column = 0;
    qint64 m_columnEnd = 0; // absolute frame where the current column ends
    qint64 m_position = 0;  // absolute frame of the next sample
    qint64 m_frames = 0;
    // Low: LP(f1)×2. Rest: HP(f1)×2, then mid: LP(f2)×2, high: HP(f2)×2.
    std::array<Biquad, 2> m_lowPass1{};
    std::array<Biquad, 2> m_highPass1{};
    std::array<Biquad, 2> m_lowPass2{};
    std::array<Biquad, 2> m_highPass2{};
    Accumulator m_accumulator;
};

} // namespace selecta::audio
