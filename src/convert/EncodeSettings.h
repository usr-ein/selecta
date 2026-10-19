// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"

#include <QString>

namespace selecta::convert {

enum class OutputFormat {
    Mp3,       // libmp3lame, genuine LAME Info tag (default Inbox format, CONV-2)
    Aiff,      // FFmpeg AIFF muxer, big-endian PCM (the uncompressed filing format)
    Flac,      // FFmpeg FLAC encoder
    Wav,       // Selecta's own WAVE_FORMAT_PCM writer
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
    // Output rate in Hz; 0 picks automatically: the source rate when the format can carry it, else 44.1 kHz (MP3
    // above 48 kHz, D-4). PCM and FLAC outputs above maxSampleRate are resampled to 44.1 or 48 kHz families' largest
    // allowed rate. Never upsampled automatically.
    int sampleRate = 0;
    // PCM/FLAC word length: 16 or 24; 0 keeps the source depth (lossy sources and 8-bit become 16; 32-bit and float
    // become 24). TPDF dither when reducing to 16-bit.
    int bitDepth = 0;
    // The hardware ceiling for lossless outputs (D-3: CDJ-3000/NXS2 play ≤ 96 kHz / 24-bit).
    int maxSampleRate = 96000;
};

// The concrete output a source gets under `settings` (shared by the Transcoder and the QualityAdvisor, so the advice
// always describes what the conversion does).
struct ResolvedOutput
{
    int sampleRate = 0;
    int bitDepth = 0;       // 0 for MP3
    bool resample = false;  // libsoxr runs (I-RESAMPLE)
    bool dither = false;    // TPDF dither to 16-bit (I-DITHER)
    bool upsample = false;  // only when settings ask for a higher rate explicitly (W-UPSAMPLE)
    bool upDepth = false;   // only when settings ask for a larger word length explicitly (W-UPDEPTH)
};
ResolvedOutput resolveOutput(const audio::StreamInfo &source, const EncodeSettings &settings);

// "MP3 320 kbps CBR", "MP3 V0", "AIFF", "FLAC", "WAV", "Original audio".
QString describeSettings(const EncodeSettings &settings);
// File extension without the dot: "mp3", "aiff", "flac", "wav"; empty for KeepAudio (the source's extension).
QString fileExtension(OutputFormat format);
// The LAME settings string Selecta records in ENCODING/TSSE, e.g. "LAME3.100 -b 320" or "LAME3.100 -V 0".
QString lameSettingsString(const Mp3Settings &settings);

} // namespace selecta::convert
