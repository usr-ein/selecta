// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayView>
#include <QtTypes>

#include <optional>

// Image dimensions from the header bytes alone, without decoding (the cover bound of the contract: reject over
// 10 000 px before anything decodes the image). The bytes are untrusted: every read is bounds-checked, the JPEG
// segment walk stops after kMaxScanBytes or kMaxSegments, and anything unexpected is "not an image" (nullopt) rather
// than a guess. Pure functions, thread-safe.
namespace selecta::meta::detail {

enum class ImageFormat : quint8 {
    Jpeg,
    Png,
};

struct ImageInfo
{
    ImageFormat format = ImageFormat::Jpeg;
    quint32 width = 0;
    quint32 height = 0;
};

inline constexpr qsizetype kMaxScanBytes = qsizetype{4} * 1024 * 1024; // the SOF must start within the first 4 MiB
inline constexpr int kMaxSegments = 4096;

// JPEG: the first SOFn frame header (baseline, extended, progressive, lossless, arithmetic) after SOI, walking the
// marker segments by their lengths; a scan (SOS) or EOI before any SOF, a zero height (DNL), a truncated or oversized
// segment, or a byte that isn't a marker where one must be ends the walk with nullopt.
// PNG: the signature, then an IHDR chunk of length 13 as the first chunk; width and height must be 1…2^31−1.
std::optional<ImageInfo> readImageHeader(QByteArrayView bytes);

} // namespace selecta::meta::detail
