// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

namespace selecta::audio {

enum class Container {
    Unknown,
    MpegAudio, // MP3/MP2/MP1 elementary stream, with optional ID3v2/ID3v1/APE tags
    Flac,      // native FLAC
    Wav,       // RIFF/WAVE
    Aiff,      // AIFF and AIFF-C
    Mp4,       // MP4/M4A (AAC, ALAC)
    Ogg,       // Ogg (Vorbis, Opus, FLAC)
    Other,     // anything else FFmpeg opens (WMA, APE, WavPack, DSF, …); complete files only
};

enum class Codec { Unknown, Mp3, Mp2, Mp1, Flac, Pcm, Aac, Alac, Vorbis, Opus, Other };

// The Xing/Info header in the first MPEG audio frame, with LAME's extension when present (conversion §4.4). The
// numbers are what the tag says; Selecta's own frame index is the authority on what the file contains.
struct LameTag
{
    bool isInfo = false;     // "Info" (written by LAME for CBR) rather than "Xing" (VBR)
    qint64 frames = -1;      // audio frames after this one, when the field is present
    qint64 bytes = -1;       // stream size in bytes, when present
    bool hasToc = false;     // a 100-entry seek table is present
    int vbrScale = -1;       // the Xing quality field, when present

    // LAME extension (empty encoder string when absent).
    QString encoder;         // 9 bytes, e.g. "LAME3.100", trailing spaces and NULs removed
    int revision = -1;       // tag revision (upper nibble)
    int vbrMethod = -1;      // 1 = CBR, 2 = ABR, 3–6 = VBR methods, 8 = CBR 2-pass, 9 = ABR 2-pass (lower nibble)
    int lowpassHz = -1;      // lowpass actually used, rounded to 100 Hz; 0 = none recorded
    int encoderDelay = -1;   // samples LAME added at the start
    int encoderPadding = -1; // samples LAME added at the end
    int bitrateKbps = -1;    // ABR target or CBR/VBR minimum, 255 meaning "255 or more"
    quint32 musicLength = 0; // bytes from the tag frame to the end of the audio, as written by the encoder
    bool crcValid = false;   // the tag's CRC-16 over its first 190 bytes matches (what DJ apps check)

    bool hasLameExtension() const { return !encoder.isEmpty(); }
};

// Facts from Selecta's own MPEG frame index (complete files: the whole file; growing files: the indexed prefix).
struct MpegAudioStats
{
    int version = 0;          // 1, 2, or 25 for MPEG 2.5
    int layer = 0;            // 1, 2 or 3
    int channelMode = 0;      // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    qint64 frameCount = 0;    // audio frames, without the Xing/Info frame
    int minBitrateKbps = 0;
    int maxBitrateKbps = 0;
    qint64 skippedBytes = 0;  // junk skipped between frames while resynchronising (corruption indicator)

    bool constantBitrate() const { return frameCount > 0 && minBitrateKbps == maxBitrateKbps; }
};

// What a Decoder found in the source. Filled in by Decoder::open(); fields the format doesn't provide keep their
// defaults.
struct StreamInfo
{
    Container container = Container::Unknown;
    Codec codec = Codec::Unknown;
    QString codecName;          // FFmpeg's codec name, e.g. "mp3float", "flac", "pcm_s24le"

    int sampleRate = 0;         // Hz, of the source
    int channels = 0;           // of the source
    int bitsPerSample = 0;      // source word length for PCM, FLAC and ALAC (16, 24, 32); 0 for lossy codecs
    bool lossless = false;
    bool floatingPoint = false; // float PCM source

    qint64 totalFrames = -1;    // length in source sample frames, -1 when unknown
    bool exactLength = false;   // totalFrames comes from the stream itself (STREAMINFO, chunk sizes, LAME tag, index);
                                // false: estimated from size and bitrate
    int bitrateKbps = 0;        // average bitrate of the audio data

    qint64 audioOffset = 0;     // first byte of audio data (after ID3v2, FLAC metadata, RIFF header…)
    qint64 audioBytes = -1;     // size of the audio data, -1 when unknown

    bool progressive = false;   // this format can be decoded while the file downloads
    bool exactSeek = false;     // Decoder::seek() lands on the exact sample

    std::optional<LameTag> lameTag;      // MPEG audio with a Xing/Info header
    std::optional<MpegAudioStats> mpeg;  // MPEG audio indexed by Selecta

    // Duration in seconds, or -1 when unknown.
    double durationSeconds() const
    {
        return totalFrames >= 0 && sampleRate > 0 ? static_cast<double>(totalFrames) / sampleRate : -1.0;
    }
};

QString containerName(Container container); // "MP3", "FLAC", "WAV", "AIFF", "MP4", "Ogg", "Other", "Unknown"
QString codecName(Codec codec);             // "MP3", "MP2", "MP1", "FLAC", "PCM", "AAC", "ALAC", "Vorbis", "Opus", …

} // namespace selecta::audio
