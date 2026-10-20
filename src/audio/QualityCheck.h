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
    LikelyFromLossy,      // a lossless file whose spectrum stops at or below about 19 kHz (192 kbps MP3 and below)
    LikelyUpconvertedMp3, // an MP3 at ≥ 256 kbps whose spectrum stops at or below about 17.5 kHz (≤ 160 kbps)
    LikelyReencoded,      // an MP3 whose LAME tag records a lowpass more than 1.5 kHz above the measured cut-off
    LikelyPadded24Bit,    // a 20/24-bit or float file whose every sample sits on the 16-bit grid (exact)
    LikelyUpsampled,      // a file above 48 kHz with no content above 22.05/24 kHz
};

struct QualityReport
{
    // Spectral cut-off: the highest frequency, in Hz, whose long-term level is within 24 dB of the 14–16 kHz reference
    // band (everything above it stays below); within ~300 Hz of LAME's lowpass for LAME encodes. -1 when it can't be
    // measured: silence, under 2 seconds, a rate below 32 kHz, or a reference band holding only the noise floor (more
    // than 80 dB below the loudest part of the spectrum). Spectral findings need 44.1 kHz or more.
    int cutoffHz = -1;
    // What the cut-off suggests about the source, from LAME's lowpass table (128 kbps → 17 kHz, …, 320 → 20.5 kHz),
    // when the cut-off is near one of its entries; set only together with LikelyFromLossy, LikelyUpconvertedMp3 or
    // LikelyReencoded.
    std::optional<int> likelySourceKbps;
    // The lowpass a LAME tag records, for MP3s that have one.
    std::optional<int> lameLowpassHz;

    int sourceBitDepth = 0; // as declared (PCM, FLAC, ALAC); 0 for lossy codecs
    // 16 when every sample of a 20/24-bit or float file sits on the 16-bit grid (and isn't all silence); else the
    // declared depth. 32-bit integer files aren't checked: a float sample can't hold them exactly.
    int effectiveBitDepth = 0;
    int sampleRate = 0;
    // For rates above 48 kHz: the highest frequency within 70 dB of the reference band (genuine recordings carry
    // harmonics and noise far up; upsampled ones stop at the resampler's 20–22 kHz filter); -1 otherwise.
    int contentBandwidthHz = -1;

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
Result<QualityReport> analyzeQuality(const std::shared_ptr<ByteSource> &source, QualityOptions options = {},
                                     const CancellationFlag *cancel = nullptr);

} // namespace selecta::audio
