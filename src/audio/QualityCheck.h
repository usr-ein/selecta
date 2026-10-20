// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/Error.h"
#include "audio/Spectrogram.h"
#include "audio/StreamInfo.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringView>

#include <memory>
#include <optional>

class QThreadPool;

// Quality checks (PREV-9, references/audio-preview-and-waveform.md §9): is a file what it claims to be? Spectral
// verdicts are heuristics and are always worded "likely …", never "fake": old recordings, lo-fi productions and some
// masters are genuinely band-limited. The bit-depth check is exact.
namespace selecta::audio {

// Version of the checks and the spectrogram (FFT, windows, thresholds, level mapping). Cached reports from another
// version are recomputed (AnalysisCache::loadQuality()).
inline constexpr int kQualityVersion = 1;

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

    // The spectrogram, when QualityOptions::spectrogram asked for one (also for files whose cut-off can't be measured:
    // short, quiet or low-rate ones). Computed in the same decoding pass; it doesn't change any other field.
    std::optional<Spectrogram> spectrogram;

    bool hasFinding(QualityFinding finding) const { return findings.contains(finding); }

    friend bool operator==(const QualityReport &, const QualityReport &) = default;
};

// UI copy for a finding, always phrased as "likely": e.g. "Likely from a lossy source (≈ 192 kbps MP3)". Uses the
// report for the numbers.
QString describeFinding(QualityFinding finding, const QualityReport &report);
// A short form for badges, from what is kept without the report (an Inbox item keeps the findings and the bitrate):
// "Likely from a ≈ 192 kbps MP3", "Likely from a lossy file", "Likely upconverted from ≈ 128 kbps", "Likely
// re-encoded from a lossy file", "Likely 16-bit, padded", "Likely upsampled".
QString findingBadge(QualityFinding finding, std::optional<int> likelySourceKbps = {});
// A stable name for storing a finding outside the cache ("from-lossy", "upconverted-mp3", "reencoded",
// "padded-24-bit", "upsampled"), and back: nullopt for a name this version doesn't know.
QString findingName(QualityFinding finding);
std::optional<QualityFinding> findingFromName(QStringView name);

struct QualityOptions
{
    int spectrumFrames = 200; // FFT frames (4 096 points, Hann) spread evenly over the file, 1…10 000
    // Also compute QualityReport::spectrogram, with these dimensions, in the same decoding pass.
    bool spectrogram = false;
    SpectrogramOptions spectrogramOptions;
    // Parallel decoding of a complete file in chunks, as in AnalysisOptions: 0 uses QThread::idealThreadCount(), 1
    // decodes sequentially. Chunks are used when the decoder seeks exactly (MP3, FLAC, WAV, AIFF, M4A and most
    // FFmpeg formats) and each gets at least `minChunkSeconds` of audio. The chunks never depend on the spectrogram
    // options, so the checks' results are the same with and without a spectrogram.
    int maxThreads = 0;
    // Pool for the chunk tasks; nullptr uses QThreadPool::globalInstance(). The calling thread always takes part, so
    // calling from a task of the same pool can't deadlock.
    QThreadPool *pool = nullptr;
    int minChunkSeconds = 20;
};

// Decodes `source` (complete) once, at its own rate and channel count, runs every check and, when asked, builds the
// spectrogram on the way. Memory stays bounded whatever the file's length: the audio streams through (a few FFT
// windows of history per chunk), never held whole. Blocks until done: call it from a worker, never from the UI or
// engine thread. Reentrant: concurrent calls on different sources (or the same one) are independent.
//
// Failure codes: those of Decoder::open()/read(); InvalidArgument for a missing or incomplete source or options out
// of range (spectrumFrames, the spectrogram's dimensions); Cancelled when `cancel` is set (checked between buffers of
// at most 8 192 frames in every chunk).
Result<QualityReport> analyzeQuality(const std::shared_ptr<ByteSource> &source, const QualityOptions &options = {},
                                     const CancellationFlag *cancel = nullptr);

} // namespace selecta::audio

Q_DECLARE_METATYPE(selecta::audio::QualityReport)
