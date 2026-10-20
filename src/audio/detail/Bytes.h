// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Bounds-checked integer reads from byte spans, for the container parsers. Every accessor takes an offset and returns
// false (or 0 from the *Or helpers) when the read would leave the span: parsers never index untrusted data blindly.
namespace selecta::audio::detail {

using ByteSpan = std::span<const quint8>;

inline bool has(ByteSpan bytes, std::size_t offset, std::size_t count)
{
    return offset <= bytes.size() && count <= bytes.size() - offset;
}

inline quint32 be16(ByteSpan b, std::size_t o)
{
    return (quint32{b[o]} << 8U) | b[o + 1];
}
inline quint32 be24(ByteSpan b, std::size_t o)
{
    return (quint32{b[o]} << 16U) | (quint32{b[o + 1]} << 8U) | b[o + 2];
}
inline quint32 be32(ByteSpan b, std::size_t o)
{
    return (quint32{b[o]} << 24U) | (quint32{b[o + 1]} << 16U) | (quint32{b[o + 2]} << 8U) | b[o + 3];
}
inline quint64 be64(ByteSpan b, std::size_t o)
{
    return (quint64{be32(b, o)} << 32U) | be32(b, o + 4);
}
inline quint32 le16(ByteSpan b, std::size_t o)
{
    return quint32{b[o]} | (quint32{b[o + 1]} << 8U);
}
inline quint32 le32(ByteSpan b, std::size_t o)
{
    return quint32{b[o]} | (quint32{b[o + 1]} << 8U) | (quint32{b[o + 2]} << 16U) | (quint32{b[o + 3]} << 24U);
}
inline quint64 le64(ByteSpan b, std::size_t o)
{
    return quint64{le32(b, o)} | (quint64{le32(b, o + 4)} << 32U);
}

// Four-character code comparison, e.g. fourcc(b, 0, "RIFF").
inline bool fourcc(ByteSpan b, std::size_t o, std::string_view tag)
{
    if (tag.size() != 4 || !has(b, o, 4)) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (b[o + i] != static_cast<quint8>(tag[i])) {
            return false;
        }
    }
    return true;
}

} // namespace selecta::audio::detail
