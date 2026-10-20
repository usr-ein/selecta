// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ImageHeader.h"

#include <algorithm>
#include <array>

namespace selecta::meta::detail {

namespace {

// A byte at `pos`, or -1 beyond the end.
int byteAt(QByteArrayView bytes, qsizetype pos)
{
    if (pos < 0 || pos >= bytes.size()) {
        return -1;
    }
    return static_cast<unsigned char>(bytes[pos]);
}

// A big-endian 16-bit value at `pos`, or nullopt when it doesn't fit.
std::optional<quint32> be16(QByteArrayView bytes, qsizetype pos)
{
    const int hi = byteAt(bytes, pos);
    const int lo = byteAt(bytes, pos + 1);
    if (hi < 0 || lo < 0) {
        return std::nullopt;
    }
    return (static_cast<quint32>(hi) << 8U) | static_cast<quint32>(lo);
}

std::optional<quint32> be32(QByteArrayView bytes, qsizetype pos)
{
    const std::optional<quint32> hi = be16(bytes, pos);
    const std::optional<quint32> lo = be16(bytes, pos + 2);
    if (!hi.has_value() || !lo.has_value()) {
        return std::nullopt;
    }
    return (*hi << 16U) | *lo;
}

bool isStartOfFrame(int marker)
{
    // SOF0–SOF15 are C0–CF, except C4 (DHT), C8 (JPG, reserved) and CC (DAC).
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

bool isStandalone(int marker)
{
    // Markers without a length: TEM, RST0–RST7, SOI.
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8);
}

std::optional<ImageInfo> readJpeg(QByteArrayView bytes)
{
    const qsizetype limit = std::min(bytes.size(), kMaxScanBytes);
    qsizetype pos = 2; // after SOI
    for (int segment = 0; segment < kMaxSegments; ++segment) {
        if (pos >= limit || byteAt(bytes, pos) != 0xFF) {
            return std::nullopt; // a marker must start here
        }
        while (byteAt(bytes, pos) == 0xFF) { // fill bytes
            if (++pos >= limit) {
                return std::nullopt;
            }
        }
        const int marker = byteAt(bytes, pos++);
        if (marker < 0 || marker == 0x00) {
            return std::nullopt; // stuffed byte outside entropy-coded data: not a marker
        }
        if (isStandalone(marker)) {
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return std::nullopt; // EOI or a scan before any frame header
        }
        const std::optional<quint32> length = be16(bytes, pos);
        if (!length.has_value() || *length < 2 || pos + static_cast<qsizetype>(*length) > bytes.size()) {
            return std::nullopt; // zero-length, truncated or oversized segment
        }
        if (isStartOfFrame(marker)) {
            if (*length < 8) {
                return std::nullopt; // too short to hold precision, height, width and the component count
            }
            const std::optional<quint32> height = be16(bytes, pos + 3);
            const std::optional<quint32> width = be16(bytes, pos + 5);
            if (!height.has_value() || !width.has_value() || *height == 0 || *width == 0) {
                return std::nullopt; // a height defined later by DNL isn't supported
            }
            return ImageInfo{ImageFormat::Jpeg, *width, *height};
        }
        pos += static_cast<qsizetype>(*length);
    }
    return std::nullopt;
}

std::optional<ImageInfo> readPng(QByteArrayView bytes)
{
    // 8-byte signature, then IHDR: length (4) = 13, type (4) = "IHDR", width (4), height (4).
    constexpr qsizetype kIhdrLengthAt = 8;
    if (be32(bytes, kIhdrLengthAt) != 13U || bytes.size() < 8 + 8 + 13) {
        return std::nullopt;
    }
    if (bytes.sliced(12, 4) != QByteArrayView("IHDR")) {
        return std::nullopt;
    }
    const std::optional<quint32> width = be32(bytes, 16);
    const std::optional<quint32> height = be32(bytes, 20);
    constexpr quint32 kMaxDimension = 0x7FFFFFFFU; // PNG: 2^31 − 1
    if (!width.has_value() || !height.has_value() || *width == 0 || *height == 0 || *width > kMaxDimension ||
        *height > kMaxDimension) {
        return std::nullopt;
    }
    return ImageInfo{ImageFormat::Png, *width, *height};
}

} // namespace

std::optional<ImageInfo> readImageHeader(QByteArrayView bytes)
{
    if (bytes.size() >= 3 && byteAt(bytes, 0) == 0xFF && byteAt(bytes, 1) == 0xD8 && byteAt(bytes, 2) == 0xFF) {
        return readJpeg(bytes);
    }
    static constexpr std::array<char, 8> kPngSignature = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (bytes.size() >= 8 && bytes.first(8) == QByteArrayView(kPngSignature.data(), 8)) {
        return readPng(bytes);
    }
    return std::nullopt;
}

} // namespace selecta::meta::detail
