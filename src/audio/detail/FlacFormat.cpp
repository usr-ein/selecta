// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/FlacFormat.h"

#include "audio/detail/Crc.h"

#include <algorithm>

namespace selecta::audio::detail {

std::optional<FlacStreamInfo> parseFlacStreamInfo(ByteSpan body)
{
    if (body.size() < 34) {
        return std::nullopt;
    }
    FlacStreamInfo info;
    info.minBlockSize = static_cast<int>(be16(body, 0));
    info.maxBlockSize = static_cast<int>(be16(body, 2));
    info.minFrameSize = static_cast<int>(be24(body, 4));
    info.maxFrameSize = static_cast<int>(be24(body, 7));
    // 20 bits rate, 3 bits channels-1, 5 bits bps-1, 36 bits total samples.
    const quint64 packed = be64(body, 10);
    info.sampleRate = static_cast<int>(packed >> 44U);
    info.channels = static_cast<int>((packed >> 41U) & 0x7U) + 1;
    info.bitsPerSample = static_cast<int>((packed >> 36U) & 0x1FU) + 1;
    info.totalSamples = static_cast<qint64>(packed & 0xFFFFFFFFFULL);
    std::copy_n(body.begin(), 34, info.raw.begin());
    if (info.sampleRate <= 0 || info.sampleRate > 655350 || info.maxBlockSize < 16 || info.minBlockSize < 16 ||
        info.minBlockSize > info.maxBlockSize || info.bitsPerSample < 4) {
        return std::nullopt;
    }
    return info;
}

std::optional<FlacFrameHeader> parseFlacFrameHeader(ByteSpan bytes)
{
    if (bytes.size() < 6 || bytes[0] != 0xFF || (bytes[1] & 0xFEU) != 0xF8) {
        return std::nullopt;
    }
    FlacFrameHeader h;
    h.variableBlockSize = (bytes[1] & 1U) != 0;
    const unsigned blockCode = bytes[2] >> 4U;
    const unsigned rateCode = bytes[2] & 0xFU;
    const unsigned channelCode = bytes[3] >> 4U;
    const unsigned sizeCode = (static_cast<unsigned>(bytes[3]) >> 1U) & 7U;
    if (blockCode == 0 || rateCode == 15 || channelCode > 10 || sizeCode == 3 || (bytes[3] & 1U) != 0) {
        return std::nullopt;
    }
    // UTF-8-style coded number: up to 6 bytes for frame numbers, 7 for sample numbers.
    std::size_t pos = 4;
    const quint8 lead = bytes[pos];
    int extra = 0;
    quint64 value = 0;
    if ((lead & 0x80U) == 0) {
        value = lead;
    } else if ((lead & 0xE0U) == 0xC0) {
        value = lead & 0x1FU;
        extra = 1;
    } else if ((lead & 0xF0U) == 0xE0) {
        value = lead & 0x0FU;
        extra = 2;
    } else if ((lead & 0xF8U) == 0xF0) {
        value = lead & 0x07U;
        extra = 3;
    } else if ((lead & 0xFCU) == 0xF8) {
        value = lead & 0x03U;
        extra = 4;
    } else if ((lead & 0xFEU) == 0xFC) {
        value = lead & 0x01U;
        extra = 5;
    } else if (lead == 0xFE && h.variableBlockSize) {
        value = 0;
        extra = 6;
    } else {
        return std::nullopt;
    }
    ++pos;
    for (int i = 0; i < extra; ++i) {
        if (pos >= bytes.size() || (bytes[pos] & 0xC0U) != 0x80) {
            return std::nullopt;
        }
        value = (value << 6U) | (bytes[pos] & 0x3FU);
        ++pos;
    }
    h.codedNumber = static_cast<qint64>(value);

    if (blockCode == 1) {
        h.blockSize = 192;
    } else if (blockCode <= 5) {
        h.blockSize = static_cast<int>(576U << (blockCode - 2U));
    } else if (blockCode == 6) {
        if (pos + 1 > bytes.size()) {
            return std::nullopt;
        }
        h.blockSize = bytes[pos] + 1;
        pos += 1;
    } else if (blockCode == 7) {
        if (pos + 2 > bytes.size()) {
            return std::nullopt;
        }
        h.blockSize = static_cast<int>(be16(bytes, pos)) + 1;
        pos += 2;
    } else {
        h.blockSize = static_cast<int>(256U << (blockCode - 8U));
    }

    static constexpr std::array<int, 12> kRates = {0,     88200, 176400, 192000, 8000,  16000,
                                                   22050, 24000, 32000,  44100,  48000, 96000};
    if (rateCode < 12) {
        h.sampleRate = kRates.at(rateCode);
    } else if (rateCode == 12) {
        if (pos + 1 > bytes.size()) {
            return std::nullopt;
        }
        h.sampleRate = bytes[pos] * 1000;
        pos += 1;
    } else {
        if (pos + 2 > bytes.size()) {
            return std::nullopt;
        }
        const int v = static_cast<int>(be16(bytes, pos));
        h.sampleRate = rateCode == 13 ? v : v * 10;
        pos += 2;
    }
    h.channels = channelCode < 8 ? static_cast<int>(channelCode) + 1 : 2;
    static constexpr std::array<int, 8> kSizes = {0, 8, 12, 0, 16, 20, 24, 32};
    h.bitsPerSample = kSizes.at(sizeCode);

    if (pos >= bytes.size()) {
        return std::nullopt;
    }
    if (crc8Flac(bytes.first(pos)) != bytes[pos]) {
        return std::nullopt;
    }
    h.headerBytes = static_cast<int>(pos) + 1;
    return h;
}

qint64 flacFirstSample(const FlacFrameHeader &header, int fixedBlockSize)
{
    return header.variableBlockSize ? header.codedNumber : header.codedNumber * fixedBlockSize;
}

bool flacHeaderMatches(const FlacFrameHeader &header, const FlacStreamInfo &info)
{
    return (header.sampleRate == 0 || header.sampleRate == info.sampleRate) && header.channels == info.channels &&
           (header.bitsPerSample == 0 || header.bitsPerSample == info.bitsPerSample) &&
           header.blockSize <= info.maxBlockSize;
}

} // namespace selecta::audio::detail
