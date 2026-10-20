// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>

// Synthetic image headers for tests: just enough structure for a header reader, no pixel data worth decoding.
namespace selecta::meta::testing {

inline QByteArray be16Bytes(int value)
{
    const auto bits = static_cast<quint32>(value);
    QByteArray out(2, '\0');
    out[0] = static_cast<char>((bits >> 8U) & 0xFFU);
    out[1] = static_cast<char>(bits & 0xFFU);
    return out;
}

inline QByteArray be32Bytes(quint32 value)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i) {
        const auto shift = static_cast<quint32>(24 - (8 * i));
        out[i] = static_cast<char>((value >> shift) & 0xFFU);
    }
    return out;
}

// A JPEG segment: FF <marker> <length> <payload>.
inline QByteArray jpegSegment(unsigned char marker, const QByteArray &payload)
{
    QByteArray segment;
    segment += '\xFF';
    segment += static_cast<char>(marker);
    segment += be16Bytes(static_cast<int>(payload.size()) + 2);
    segment += payload;
    return segment;
}

// SOI, a JFIF APP0, an optional padding APP1 of `exifBytes`, SOFn with the size, a short SOS and EOI.
inline QByteArray jpeg(int width, int height, unsigned char sof = 0xC0, int exifBytes = 0)
{
    QByteArray bytes("\xFF\xD8", 2);
    bytes += jpegSegment(0xE0, QByteArray("JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14));
    if (exifBytes > 0) {
        QByteArray exif("Exif\0\0", 6);
        exif += QByteArray(exifBytes, 'e');
        bytes += jpegSegment(0xE1, exif);
    }
    QByteArray frame;
    frame += '\x08'; // precision
    frame += be16Bytes(height);
    frame += be16Bytes(width);
    frame += '\x03'; // components
    frame += QByteArray("\x01\x22\x00\x02\x11\x01\x03\x11\x01", 9);
    bytes += jpegSegment(sof, frame);
    bytes += jpegSegment(0xDA, QByteArray("\x03\x01\x00\x02\x11\x03\x11\x00\x3F\x00", 10));
    bytes += QByteArray(64, '\x55'); // "entropy-coded data"
    bytes += QByteArray("\xFF\xD9", 2);
    return bytes;
}

// The PNG signature and an IHDR chunk with the size (CRC not checked by the reader), then an IEND.
inline QByteArray png(quint32 width, quint32 height)
{
    QByteArray bytes("\x89PNG\r\n\x1a\n", 8);
    bytes += be32Bytes(13);
    bytes += "IHDR";
    bytes += be32Bytes(width);
    bytes += be32Bytes(height);
    bytes += QByteArray("\x08\x06\x00\x00\x00", 5);
    bytes += be32Bytes(0); // CRC
    bytes += be32Bytes(0);
    bytes += "IEND";
    bytes += be32Bytes(0);
    return bytes;
}

} // namespace selecta::meta::testing
