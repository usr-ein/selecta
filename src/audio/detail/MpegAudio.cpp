// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/MpegAudio.h"

#include "audio/detail/Crc.h"

#include <array>

namespace selecta::audio::detail {
namespace {

// [mpeg1 ? 0 : 1][layer - 1][index]
constexpr std::array<std::array<std::array<int, 16>, 3>, 2> kBitrates = {{
    {{
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    }},
    {{
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    }},
}};

constexpr std::array<int, 3> kRatesMpeg1 = {44100, 48000, 32000};

constexpr quint32 kXingFlagFrames = 0x1;
constexpr quint32 kXingFlagBytes = 0x2;
constexpr quint32 kXingFlagToc = 0x4;
constexpr quint32 kXingFlagQuality = 0x8;
constexpr std::size_t kLameExtensionBytes = 36;

QString trimmedEncoder(ByteSpan bytes, std::size_t offset)
{
    QByteArray text;
    for (std::size_t i = 0; i < 9; ++i) {
        const quint8 c = bytes[offset + i];
        if (c == 0) {
            break;
        }
        // The field is ASCII in practice; anything else is replaced so a crafted tag can't inject control text.
        text.append(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    return QString::fromLatin1(text).trimmed();
}

} // namespace

std::optional<MpegHeader> parseMpegHeader(quint32 word)
{
    if ((word & 0xFFE00000U) != 0xFFE00000U) {
        return std::nullopt;
    }
    const quint32 versionBits = (word >> 19U) & 3U;
    const quint32 layerBits = (word >> 17U) & 3U;
    const quint32 bitrateIndex = (word >> 12U) & 0xFU;
    const quint32 rateIndex = (word >> 10U) & 3U;
    if (versionBits == 1 || layerBits == 0 || bitrateIndex == 0xF || rateIndex == 3) {
        return std::nullopt;
    }
    MpegHeader h;
    if (versionBits == 3) {
        h.version = 1;
    } else if (versionBits == 2) {
        h.version = 2;
    } else {
        h.version = 25;
    }
    h.layer = 4 - static_cast<int>(layerBits);
    h.hasCrc = ((word >> 16U) & 1U) == 0;
    h.padding = ((word >> 9U) & 1U) != 0;
    h.channelMode = static_cast<int>((word >> 6U) & 3U);
    h.channels = h.channelMode == 3 ? 1 : 2;
    const bool mpeg1 = h.version == 1;
    const int base = kRatesMpeg1.at(rateIndex);
    // MPEG-2 halves the MPEG-1 rates, MPEG 2.5 quarters them.
    h.sampleRate = mpeg1 ? base : base / (h.version == 2 ? 2 : 4);
    h.bitrateKbps = kBitrates.at(mpeg1 ? 0 : 1).at(static_cast<std::size_t>(h.layer - 1)).at(bitrateIndex);
    if (h.layer == 1) {
        h.samplesPerFrame = 384;
    } else if (h.layer == 2 || mpeg1) {
        h.samplesPerFrame = 1152;
    } else {
        h.samplesPerFrame = 576;
    }
    if (h.bitrateKbps > 0) {
        const int pad = h.padding ? 1 : 0;
        const int bps = h.bitrateKbps * 1000;
        if (h.layer == 1) {
            h.frameBytes = (((12 * bps) / h.sampleRate) + pad) * 4;
        } else if (h.layer == 3 && !mpeg1) {
            h.frameBytes = ((72 * bps) / h.sampleRate) + pad;
        } else {
            h.frameBytes = ((144 * bps) / h.sampleRate) + pad;
        }
    }
    return h;
}

bool sameMpegStream(const MpegHeader &a, const MpegHeader &b)
{
    return a.version == b.version && a.layer == b.layer && a.sampleRate == b.sampleRate && a.channels == b.channels;
}

int xingOffset(const MpegHeader &header)
{
    // Side information size: MPEG-1 32 bytes stereo / 17 mono; MPEG-2/2.5 17 stereo / 9 mono (Layer III).
    const bool mono = header.channels == 1;
    int sideInfo = mono ? 9 : 17;
    if (header.version == 1) {
        sideInfo = mono ? 17 : 32;
    }
    return 4 + (header.hasCrc ? 2 : 0) + sideInfo;
}

std::optional<LameTag> parseXingFrame(ByteSpan frame, const MpegHeader &header)
{
    if (header.layer != 3) {
        return std::nullopt;
    }
    const auto start = static_cast<std::size_t>(xingOffset(header));
    if (!has(frame, start, 8) || (!fourcc(frame, start, "Xing") && !fourcc(frame, start, "Info"))) {
        return std::nullopt;
    }
    LameTag tag;
    tag.isInfo = fourcc(frame, start, "Info");
    const quint32 flags = be32(frame, start + 4);
    std::size_t pos = start + 8;
    if ((flags & kXingFlagFrames) != 0) {
        if (!has(frame, pos, 4)) {
            return std::nullopt;
        }
        tag.frames = be32(frame, pos);
        pos += 4;
    }
    if ((flags & kXingFlagBytes) != 0) {
        if (!has(frame, pos, 4)) {
            return std::nullopt;
        }
        tag.bytes = be32(frame, pos);
        pos += 4;
    }
    if ((flags & kXingFlagToc) != 0) {
        if (!has(frame, pos, 100)) {
            return std::nullopt;
        }
        tag.hasToc = true;
        pos += 100;
    }
    if ((flags & kXingFlagQuality) != 0) {
        if (!has(frame, pos, 4)) {
            return std::nullopt;
        }
        tag.vbrScale = static_cast<int>(be32(frame, pos));
        pos += 4;
    }
    if (!has(frame, pos, kLameExtensionBytes)) {
        return tag; // a bare Xing header
    }
    const QString encoder = trimmedEncoder(frame, pos);
    if (encoder.isEmpty()) {
        return tag;
    }
    tag.encoder = encoder;
    tag.revision = static_cast<int>(frame[pos + 9] >> 4U);
    tag.vbrMethod = static_cast<int>(frame[pos + 9] & 0xFU);
    tag.lowpassHz = frame[pos + 10] * 100;
    // pos + 11: peak (4), radio gain (2), audiophile gain (2), flags/ATH (1).
    tag.bitrateKbps = frame[pos + 20];
    const quint32 delays = be24(frame, pos + 21);
    tag.encoderDelay = static_cast<int>(delays >> 12U);
    tag.encoderPadding = static_cast<int>(delays & 0xFFFU);
    // pos + 24: misc (1), MP3 gain (1), preset/surround (2).
    tag.musicLength = be32(frame, pos + 28);
    // pos + 32: music CRC (2); pos + 34: tag CRC over everything before it.
    const auto stored = static_cast<quint16>(be16(frame, pos + 34));
    tag.crcValid = crc16Lame(frame.first(pos + 34)) == stored;
    return tag;
}

bool parseVbriFrame(ByteSpan frame, qint64 *frames)
{
    // Always 32 bytes after the 4-byte header.
    constexpr std::size_t offset = 4 + 32;
    if (!has(frame, offset, 18) || !fourcc(frame, offset, "VBRI") || be16(frame, offset + 4) != 1) {
        return false;
    }
    *frames = be32(frame, offset + 14);
    return true;
}

qint64 id3v2TagSize(ByteSpan bytes)
{
    if (!has(bytes, 0, 10) || bytes[0] != 'I' || bytes[1] != 'D' || bytes[2] != '3') {
        return 0;
    }
    const quint8 major = bytes[3];
    if (major < 2 || major > 4 || bytes[4] == 0xFF) {
        return -1;
    }
    quint32 size = 0;
    for (std::size_t i = 6; i < 10; ++i) {
        if ((bytes[i] & 0x80U) != 0) {
            return -1;
        }
        size = (size << 7U) | bytes[i];
    }
    const bool footer = major == 4 && (bytes[5] & 0x10U) != 0;
    return 10 + static_cast<qint64>(size) + (footer ? 10 : 0);
}

int lameLowpassForBitrate(int kbps)
{
    // libmp3lame/lame.c optimum_bandwidth (CBR/ABR), conversion §2.1.
    struct Entry
    {
        int kbps;
        int hz;
    };
    constexpr std::array<Entry, 17> table = {{{8, 2000},
                                              {16, 3700},
                                              {24, 3900},
                                              {32, 5500},
                                              {40, 7000},
                                              {48, 7500},
                                              {56, 10000},
                                              {64, 11000},
                                              {80, 13500},
                                              {96, 15100},
                                              {112, 15600},
                                              {128, 17000},
                                              {160, 17500},
                                              {192, 18600},
                                              {224, 19400},
                                              {256, 19700},
                                              {320, 20500}}};
    for (const Entry &entry : table) {
        if (entry.kbps == kbps) {
            return entry.hz;
        }
    }
    return 0;
}

} // namespace selecta::audio::detail
