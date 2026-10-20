// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"
#include "audio/detail/Bytes.h"

#include <QtGlobal>

#include <optional>

// MPEG audio (MP1/MP2/MP3) frame headers, the Xing/Info/LAME tag and ID3 tag sizes. Pure functions over bytes.
namespace selecta::audio::detail {

struct MpegHeader
{
    int version = 0;     // 1, 2, or 25 (MPEG 2.5)
    int layer = 0;       // 1, 2, 3
    bool hasCrc = false; // protection bit clear: a 16-bit CRC follows the header
    int bitrateKbps = 0; // 0: free format
    int sampleRate = 0;
    bool padding = false;
    int channelMode = 0; // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    int channels = 0;
    int frameBytes = 0; // 0 for free format (unknown from the header alone)
    int samplesPerFrame = 0;
};

// Parses a 32-bit big-endian header word with FFmpeg's validity rules (ff_mpa_check_header): sync, version, layer,
// bitrate and sample-rate fields. nullopt when invalid.
std::optional<MpegHeader> parseMpegHeader(quint32 word);

// Frames of one stream share version, layer, sample rate and mono/stereo-ness; a header that differs is a false sync.
bool sameMpegStream(const MpegHeader &a, const MpegHeader &b);

// Offset of the Xing/Info tag inside a frame, after the header (and its CRC) and side information.
int xingOffset(const MpegHeader &header);

// The Xing/Info header (and LAME extension) of a complete first frame, or nullopt when the frame holds none. `frame`
// starts at the frame header and spans the whole frame.
std::optional<LameTag> parseXingFrame(ByteSpan frame, const MpegHeader &header);

// A Fraunhofer VBRI header in a first frame: true (with its frame count) when present. Such a frame carries no audio.
bool parseVbriFrame(ByteSpan frame, qint64 *frames);

// ID3v2 tag at the start of `bytes` (≥ 10 bytes): total size including header and footer, 0 when there is no tag,
// -1 when "ID3" is followed by an invalid header.
qint64 id3v2TagSize(ByteSpan bytes);

// LAME's lowpass for a CBR/ABR bitrate (audio §9.1), in Hz: 128 → 17 000 … 320 → 20 500. 0 when unknown.
int lameLowpassForBitrate(int kbps);

} // namespace selecta::audio::detail
