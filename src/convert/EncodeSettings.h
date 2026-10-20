// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "audio/StreamInfo.h"

#include <QString>

namespace selecta::convert {

enum class OutputFormat {
    Mp3,       // libmp3lame, genuine LAME Info tag (default Inbox format, CONV-2)
    Aiff,      // FFmpeg AIFF muxer, big-endian PCM (the uncompressed filing format)
    Flac,      // FFmpeg FLAC encoder
    Wav,       // Selecta's own WAVE_FORMAT_PCM writer
    Alac,      // FFmpeg ALAC encoder in an M4A (`ipod`) container
    KeepAudio, // the audio bytes are copied untouched and only the tags are rewritten (CONV-2 "file as-is")
};

enum class Mp3Mode { Cbr, Vbr };

struct Mp3Settings
{
    Mp3Mode mode = Mp3Mode::Cbr;
    int bitrateKbps = 320;    // CBR: 128, 160, 192, 224, 256 or 320
    int vbrQuality = 0;       // VBR: 0 (V0) … 9
    int algorithmQuality = 3; // LAME -q (0 best … 9 fastest); 3 is LAME's default and Selecta's (conversion §2.1)
};

struct EncodeSettings
{
    OutputFormat format = OutputFormat::Mp3;
    Mp3Settings mp3;
    // Output rate in Hz; 0 picks automatically. MP3: the source rate when it is 32, 44.1 or 48 kHz, else 44.1 kHz
    // (D-4; sources below 32 kHz are upsampled, which the advisor flags). Lossless: the source rate, or, above
    // maxSampleRate, the highest rate of the source's family (44.1 or 48 kHz times a power of two) that fits. An
    // explicit rate is used as given: MP3 accepts 32000, 44100 or 48000; lossless 8000…384000.
    int sampleRate = 0;
    // Lossless word length: 16 or 24; 0 picks from the source (16-bit and below → 16; 20-, 24- and 32-bit and float
    // → 24; lossy → 16). TPDF dither only when the result is 16-bit and the audio isn't already on the 16-bit grid.
    int bitDepth = 0;
    // The hardware ceiling for automatic lossless rates (D-3: CDJ-3000/NXS2 play ≤ 96 kHz / 24-bit); ≥ 44100.
    int maxSampleRate = 96000;
};

// Succeeds when `settings` are usable; otherwise fails with InvalidArgument saying why: an MP3 bitrate other than 128,
// 160, 192, 224, 256 or 320, a VBR or algorithm quality outside 0…9, a bit depth other than 0, 16 or 24, a rate outside
// the ranges above, or maxSampleRate below 44100.
audio::Result<void> checkSettings(const EncodeSettings &settings);

// The concrete output a source gets under `settings` (shared by the Transcoder and the QualityAdvisor, so the advice
// always describes what the conversion does).
struct ResolvedOutput
{
    int sampleRate = 0;
    int bitDepth = 0;      // 0 for MP3
    bool resample = false; // libsoxr runs (I-RESAMPLE)
    bool dither = false;   // TPDF dither to 16-bit (I-DITHER)
    bool upsample = false; // above the source rate: an explicit rate, or MP3 from below 32 kHz (W-UPSAMPLE)
    bool upDepth = false;  // an explicit word length above a lossless source's (W-UPDEPTH)
};
ResolvedOutput resolveOutput(const audio::StreamInfo &source, const EncodeSettings &settings);

// "MP3 320 kbps CBR", "MP3 V0", "AIFF", "FLAC", "WAV", "ALAC", "Original audio"; explicit choices are appended
// ("AIFF, 16-bit, 44.1 kHz").
QString describeSettings(const EncodeSettings &settings);
// File extension without the dot: "mp3", "aiff", "flac", "wav", "m4a"; empty for KeepAudio (the source's extension).
QString fileExtension(OutputFormat format);
// The LAME settings string Selecta records in ENCODING/TSSE, e.g. "LAME3.100 -b 320" or "LAME3.100 -V 0".
QString lameSettingsString(Mp3Settings settings);

} // namespace selecta::convert
