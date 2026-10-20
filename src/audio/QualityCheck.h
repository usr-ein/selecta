// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Error.h"
#include "audio/StreamInfo.h"

#include <QList>
#include <QString>

#include <memory>
#include <optional>

// Quality checks (PREV-9, references/audio-preview-and-waveform.md §9): is a file what it claims to be? Spectral
// verdicts are heuristics and are always worded "likely …", never "fake": old recordings, lo-fi productions and some
// masters are genuinely band-limited. The bit-depth check is exact.
namespace selecta::audio {

enum class QualityFinding {
    LikelyFromLossy,      // a lossless file whose spectrum stops at or below 19 kHz
    LikelyUpconvertedMp3, // an MP3 at ≥ 256 kbps whose spectrum stops at or below 17.5 kHz
    LikelyReencoded,      // an MP3 whose LAME tag records a lowpass well above the measured cut-off
    LikelyPadded24Bit,    // a 24- or 32-bit integer file whose low 8 bits are zero in every sample (exact)
    LikelyUpsampled,      // a file above 48 kHz with no content above 22.05/24 kHz
};

struct QualityReport
{
    // Spectral cut-off: the highest frequency, in Hz, below which the long-term spectrum stays within reach of the
    // 14–16 kHz reference band; -1 when it can't be measured (silence, under 2 seconds, or a rate below 32 kHz).
    int cutoffHz = -1;
    // What the cut-off suggests about the source, from LAME's lowpass table (128 kbps → 17 kHz, …, 320 → 20.5 kHz);
    // set only together with LikelyFromLossy or LikelyUpconvertedMp3.
    std::optional<int> likelySourceKbps;
    // The lowpass a LAME tag records, for MP3s that have one.
    std::optional<int> lameLowpassHz;

    int sourceBitDepth = 0;    // as declared (PCM, FLAC, ALAC); 0 for lossy codecs
    int effectiveBitDepth = 0; // 16 when every sample of a 24/32-bit file has its low 8 (or 16) bits clear
    int sampleRate = 0;
    int contentBandwidthHz = -1; // for rates above 48 kHz: the highest frequency with content, -1 otherwise

    QList<QualityFinding> findings;

    bool hasFinding(QualityFinding finding) const { return findings.contains(finding); }
};

// UI copy for a finding, always phrased as "likely": e.g. "Likely from a lossy source (≈ 192 kbps MP3)". Uses the
// report for the numbers.
QString describeFinding(QualityFinding finding, const QualityReport &report);

struct QualityOptions
{
    int spectrumFrames = 200; // FFT frames (4 096 points, Hann) spread evenly over the file
};

// Decodes `source` (complete) at its own rate and channel count and runs every check. Blocks: call from a worker.
// Failure codes: those of Decoder::open()/read(), InvalidArgument for an incomplete source, Cancelled.
Result<QualityReport> analyzeQuality(const std::shared_ptr<ByteSource> &source, const QualityOptions &options = {},
                                     const CancellationFlag *cancel = nullptr);

} // namespace selecta::audio
