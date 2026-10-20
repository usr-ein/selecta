// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QByteArray>
#include <QMetaType>
#include <QtGlobal>

#include <span>

// The spectrogram the inspector shows next to a quality verdict (PREV-9, references/audio-preview-and-waveform.md §9):
// a compact time × frequency grid of levels. Data only; painting (colour map, axes) is the app's job. It is computed
// by analyzeQuality() in the same decoding pass as the checks (QualityOptions::spectrogram) and cached with the report
// (AnalysisCache::storeQuality()).
namespace selecta::audio {

// Level mapping: a level byte is linear in dB, kSpectrogramFloorDb → 0 and kSpectrogramCeilingDb → 255 (0.47 dB per
// step), clamped at both ends. 0 dBFS is the level of a full-scale sine (peak ±1.0 on every channel) centred on an FFT
// bin; between bins it reads up to 1.4 dB lower (Hann scalloping). Broadband sound spreads over many FFT bins, so it
// reads lower than its RMS: white noise at -20 dBFS RMS reads about -48 dB per FFT bin, a few dB more after the maximum
// over a row's FFT bins. 16-bit quantisation noise sits near -130 dB, so digital silence and a CD's noise floor both
// map to 0.
inline constexpr double kSpectrogramFloorDb = -120.0;
inline constexpr double kSpectrogramCeilingDb = 0.0;
// Bounds on the grid (options outside them are refused; cached blobs outside them are corrupt).
inline constexpr int kSpectrogramMaxColumns = 4096;
inline constexpr int kSpectrogramMaxBins = 1024;

struct SpectrogramOptions
{
    int columns = 800; // time columns over the whole file, 1…kSpectrogramMaxColumns
    int bins = 256;    // frequency rows from 0 Hz to Nyquist, 1…kSpectrogramMaxBins
};

// A finished spectrogram: `columns()` time columns × `bins()` frequency rows of level bytes, over the whole decoded
// file, mixed down to mono ((L + R) / 2, all channels averaged).
//
// - Time: the columns divide the decoded frames evenly; column c covers frames [c·F/C, (c+1)·F/C) for F =
//   frameCount() and C = columns(). Each column is the mean power of up to 8 Hann-windowed 4 096-point FFTs spread
//   over its span (so a long file is sampled, not covered: a 6-minute track uses 6 400 FFTs for 800 columns). When a
//   file's announced length was an estimate the column edges are approximate (within one column).
// - Frequency: linear from 0 Hz to Nyquist; bin b covers [b, b + 1) · nyquistHz() / bins(), so bin 0 holds 0 Hz (DC)
//   and the top bin reaches Nyquist. Each bin is the maximum of the FFT bins that fall in it, not their mean, so a
//   lowpass edge stays sharp at any bin count.
// - Fewer columns than asked for: one column per frame at most (a file shorter than `columns` frames), and fewer when
//   the stream ended before the length its header announced (each column keeps the span it was planned with).
//   Zero columns for a file with no audio.
// - Windows reaching past the end of the audio are zero-padded, so a file shorter than one FFT window (93 ms at
//   44.1 kHz) reads lower than its true level.
//
// Layout: column-major, levels()[column · bins() + bin], each column from 0 Hz up.
//
// Immutable and implicitly shared: cheap to copy and to pass through queued signals; thread-safe for concurrent reads.
class Spectrogram
{
public:
    Spectrogram() = default;

    // Validates the dimensions (1…kSpectrogramMaxBins bins, 0…kSpectrogramMaxColumns columns, no more columns than
    // frames, levels of exactly columns × bins bytes, a positive sample rate up to 768 kHz). Failure code:
    // InvalidArgument.
    static Result<Spectrogram> fromLevels(int sampleRate, qint64 frameCount, int columns, int bins, QByteArray levels);

    bool isNull() const { return m_sampleRate == 0; }
    bool isEmpty() const { return m_columns == 0; }
    int sampleRate() const { return m_sampleRate; }
    qint64 frameCount() const { return m_frameCount; }
    double durationSeconds() const;
    int columns() const { return m_columns; }
    int bins() const { return m_bins; }

    double nyquistHz() const { return m_sampleRate / 2.0; }
    // The frequency range of `bin`: [binLowHz(bin), binLowHz(bin + 1)).
    double binLowHz(int bin) const;
    // The bin holding `hz`, clamped to 0…bins() − 1 (Nyquist itself falls in the top bin).
    int binForFrequency(double hz) const;
    // The first source frame of `column` (frameCount() for column == columns()).
    qint64 columnStartFrame(int column) const;

    // The level of one cell; 0 outside the grid.
    quint8 level(int column, int bin) const;
    // One column's bins, from 0 Hz up; empty outside the grid.
    std::span<const quint8> column(int column) const;
    // Every level, column-major.
    const QByteArray &levels() const { return m_levels; }

    // The mapping, both ways: levelForDb() rounds and clamps (NaN gives 0); dbForLevel() gives the level's dB value.
    static quint8 levelForDb(double db);
    static double dbForLevel(quint8 level);

    friend bool operator==(const Spectrogram &, const Spectrogram &) = default;

private:
    int m_sampleRate = 0;
    qint64 m_frameCount = 0;
    int m_columns = 0;
    int m_bins = 0;
    QByteArray m_levels;
};

} // namespace selecta::audio

Q_DECLARE_METATYPE(selecta::audio::Spectrogram)
