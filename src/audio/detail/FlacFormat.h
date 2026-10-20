// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/detail/Bytes.h"

#include <QtGlobal>

#include <array>
#include <optional>

// Native FLAC metadata and frame headers (RFC 9639). Pure functions over bytes.
namespace selecta::audio::detail {

struct FlacStreamInfo
{
    int minBlockSize = 0;
    int maxBlockSize = 0;
    int minFrameSize = 0; // 0: unknown
    int maxFrameSize = 0; // 0: unknown
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 totalSamples = 0;      // 0: unknown
    std::array<quint8, 34> raw{}; // the 34-byte block, used as the codec's extradata
};

// Parses a 34-byte STREAMINFO block body. nullopt when the values are invalid (rate 0, block sizes < 16, …).
std::optional<FlacStreamInfo> parseFlacStreamInfo(ByteSpan body);

struct FlacFrameHeader
{
    bool variableBlockSize = false;
    qint64 codedNumber = 0; // frame number (fixed block size) or first sample number (variable)
    int blockSize = 0;
    int sampleRate = 0; // 0: from STREAMINFO
    int channels = 0;
    int bitsPerSample = 0; // 0: from STREAMINFO
    int headerBytes = 0;   // including the CRC-8
};

// Parses a frame header at the start of `bytes` and checks its CRC-8. nullopt when it isn't a valid header (or the
// span is too short to tell; callers distinguish by checking the span length against kFlacMaxFrameHeaderBytes).
std::optional<FlacFrameHeader> parseFlacFrameHeader(ByteSpan bytes);
inline constexpr int kFlacMaxFrameHeaderBytes = 16;

// The first sample of a frame, given the stream's fixed block size (STREAMINFO's maximum) for fixed-block streams.
qint64 flacFirstSample(const FlacFrameHeader &header, int fixedBlockSize);

// True when the frame header agrees with STREAMINFO (rate, channels, word length).
bool flacHeaderMatches(const FlacFrameHeader &header, const FlacStreamInfo &info);

} // namespace selecta::audio::detail
