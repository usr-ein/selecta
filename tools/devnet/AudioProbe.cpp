// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioProbe.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace selecta::devnet {
namespace {

using slsk::FileAttributes;

constexpr qsizetype kProbeBytes = qsizetype{16} * 1024; // past any ID3v2 tag: enough for every header read here
constexpr qsizetype kId3HeaderBytes = 10;
constexpr qsizetype kId3v1Bytes = 128;

// Whether `bytes` holds `tag` at `offset`.
bool hasTag(QByteArrayView bytes, qsizetype offset, std::string_view tag)
{
    const auto length = static_cast<qsizetype>(tag.size());
    return offset >= 0 && offset + length <= bytes.size() &&
           std::memcmp(bytes.data() + offset, tag.data(), tag.size()) == 0;
}

quint32 byteAt(QByteArrayView bytes, qsizetype offset)
{
    return static_cast<quint8>(bytes.at(offset));
}

quint32 be32(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint32>(bytes.data() + offset);
}

quint32 le32(QByteArrayView bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(bytes.data() + offset);
}

quint32 le16(QByteArrayView bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(bytes.data() + offset);
}

quint32 be16(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint16>(bytes.data() + offset);
}

quint32 rounded(double value)
{
    return static_cast<quint32>(std::lround(std::clamp(value, 0.0, 4.0e9)));
}

// --- MP3 -------------------------------------------------------------------------------------------------------------

struct Mp3Frame
{
    bool mpeg1 = true;
    bool mono = false;
    quint32 bitrate = 0;    // kbps
    quint32 sampleRate = 0; // Hz
    quint32 length = 0;     // bytes
    quint32 samples = 0;    // per frame
};

// An MPEG audio Layer III frame header at `offset`.
std::optional<Mp3Frame> parseMp3Frame(QByteArrayView bytes, qsizetype offset)
{
    if (offset < 0 || offset + 4 > bytes.size()) {
        return std::nullopt;
    }
    const quint32 b1 = byteAt(bytes, offset + 1);
    const quint32 b2 = byteAt(bytes, offset + 2);
    if (byteAt(bytes, offset) != 0xFFU || (b1 & 0xE0U) != 0xE0U) {
        return std::nullopt;
    }
    const quint32 version = (b1 >> 3U) & 3U; // 0 MPEG 2.5, 1 reserved, 2 MPEG 2, 3 MPEG 1
    const quint32 layer = (b1 >> 1U) & 3U;   // 1 Layer III
    const quint32 bitrateIndex = b2 >> 4U;
    const quint32 rateIndex = (b2 >> 2U) & 3U;
    if (version == 1U || layer != 1U || bitrateIndex == 0U || bitrateIndex == 15U || rateIndex == 3U) {
        return std::nullopt;
    }
    static constexpr std::array<quint32, 15> kMpeg1Bitrates = {0,   32,  40,  48,  56,  64,  80, 96,
                                                               112, 128, 160, 192, 224, 256, 320};
    static constexpr std::array<quint32, 15> kMpeg2Bitrates = {0,  8,  16, 24,  32,  40,  48, 56,
                                                               64, 80, 96, 112, 128, 144, 160};
    static constexpr std::array<quint32, 3> kMpeg1Rates = {44100, 48000, 32000};
    Mp3Frame frame;
    frame.mpeg1 = version == 3U;
    frame.mono = (byteAt(bytes, offset + 3) >> 6U) == 3U;
    frame.bitrate = frame.mpeg1 ? kMpeg1Bitrates.at(bitrateIndex) : kMpeg2Bitrates.at(bitrateIndex);
    quint32 halvings = 0; // MPEG 2: half the MPEG 1 rate, MPEG 2.5: a quarter
    if (version == 2U) {
        halvings = 1;
    } else if (version == 0U) {
        halvings = 2;
    }
    frame.sampleRate = kMpeg1Rates.at(rateIndex) >> halvings;
    frame.samples = frame.mpeg1 ? 1152U : 576U;
    const quint32 padding = (b2 >> 1U) & 1U;
    const quint32 coefficient = frame.mpeg1 ? 144U : 72U;
    frame.length = (coefficient * frame.bitrate * 1000U / frame.sampleRate) + padding;
    return frame;
}

// The first frame header that is followed by another one (or by the end of what we read): a lone 0xFFE pattern in
// leftover tag bytes isn't a frame.
std::optional<std::pair<qsizetype, Mp3Frame>> firstMp3Frame(QByteArrayView bytes)
{
    for (qsizetype offset = 0; offset + 4 <= bytes.size(); ++offset) {
        const std::optional<Mp3Frame> frame = parseMp3Frame(bytes, offset);
        if (!frame) {
            continue;
        }
        const qsizetype next = offset + static_cast<qsizetype>(frame->length);
        if (next + 4 > bytes.size() || parseMp3Frame(bytes, next)) {
            return std::make_pair(offset, *frame);
        }
    }
    return std::nullopt;
}

struct VbrHeader
{
    bool vbr = false;
    std::optional<quint32> frames;
    std::optional<quint32> bytes;
};

// A Xing/Info header (LAME and most encoders; "Info" marks CBR) or a VBRI one (Fraunhofer), in the first frame.
std::optional<VbrHeader> vbrHeader(QByteArrayView bytes, qsizetype frameStart, const Mp3Frame &frame)
{
    qsizetype sideInfo = 0;
    if (frame.mpeg1) {
        sideInfo = frame.mono ? 17 : 32;
    } else {
        sideInfo = frame.mono ? 9 : 17;
    }
    const qsizetype xing = frameStart + 4 + sideInfo;
    if ((hasTag(bytes, xing, "Xing") || hasTag(bytes, xing, "Info")) && xing + 8 <= bytes.size()) {
        VbrHeader header;
        header.vbr = hasTag(bytes, xing, "Xing");
        const quint32 flags = be32(bytes, xing + 4);
        qsizetype field = xing + 8;
        if ((flags & 1U) != 0 && field + 4 <= bytes.size()) {
            header.frames = be32(bytes, field);
            field += 4;
        }
        if ((flags & 2U) != 0 && field + 4 <= bytes.size()) {
            header.bytes = be32(bytes, field);
        }
        return header;
    }
    const qsizetype vbri = frameStart + 4 + 32;
    if (hasTag(bytes, vbri, "VBRI") && vbri + 18 <= bytes.size()) {
        return VbrHeader{true, be32(bytes, vbri + 14), be32(bytes, vbri + 10)};
    }
    return std::nullopt;
}

} // namespace

quint64 id3v2Size(QByteArrayView head)
{
    if (head.size() < kId3HeaderBytes || !hasTag(head, 0, "ID3")) {
        return 0;
    }
    quint64 size = 0;
    for (qsizetype i = 6; i < kId3HeaderBytes; ++i) {
        const quint32 b = byteAt(head, i);
        if ((b & 0x80U) != 0) {
            return 0; // not syncsafe: not a tag
        }
        size = (size << 7U) | b;
    }
    const bool footer = (byteAt(head, 5) & 0x10U) != 0;
    return static_cast<quint64>(kId3HeaderBytes) + size + (footer ? static_cast<quint64>(kId3HeaderBytes) : 0U);
}

FileAttributes probeMp3(QByteArrayView head, quint64 audioBytes)
{
    FileAttributes attributes;
    const auto first = firstMp3Frame(head);
    if (!first) {
        return attributes;
    }
    const auto [offset, frame] = *first;
    const auto start = static_cast<quint64>(offset);
    const quint64 audio = audioBytes > start ? audioBytes - start : 0;
    const std::optional<VbrHeader> header = vbrHeader(head, offset, frame);
    if (header && header->frames && *header->frames > 0) {
        const double seconds = static_cast<double>(*header->frames) * frame.samples / frame.sampleRate;
        attributes.duration = rounded(seconds);
        attributes.vbr = header->vbr;
        const quint64 bytes = header->bytes.value_or(audio);
        attributes.bitrate = header->vbr ? rounded(static_cast<double>(bytes) * 8 / seconds / 1000) : frame.bitrate;
        return attributes;
    }
    // No frame count: constant bitrate.
    attributes.bitrate = frame.bitrate;
    attributes.vbr = header && header->vbr;
    attributes.duration = rounded(static_cast<double>(audio) * 8 / (frame.bitrate * 1000.0));
    return attributes;
}

FileAttributes probeFlac(QByteArrayView head)
{
    // "fLaC", then metadata blocks; the first is STREAMINFO (type 0, 34 bytes). Its bytes 10–17 pack the sample rate
    // (20 bits), channels − 1 (3), bits per sample − 1 (5) and the total sample count (36).
    FileAttributes attributes;
    constexpr qsizetype kStreamInfo = 8;
    if (!hasTag(head, 0, "fLaC") || head.size() < kStreamInfo + 18 || (byteAt(head, 4) & 0x7FU) != 0) {
        return attributes;
    }
    const auto packed = qFromBigEndian<quint64>(head.data() + kStreamInfo + 10);
    const auto sampleRate = static_cast<quint32>(packed >> 44U);
    if (sampleRate == 0) {
        return attributes;
    }
    const quint64 samples = packed & 0xF'FFFF'FFFFULL;
    attributes.sampleRate = sampleRate;
    attributes.bitDepth = static_cast<quint32>((packed >> 36U) & 0x1FU) + 1;
    if (samples > 0) {
        attributes.duration = rounded(static_cast<double>(samples) / sampleRate);
    }
    return attributes;
}

FileAttributes probeWav(QByteArrayView head, quint64 fileSize)
{
    FileAttributes attributes;
    if (!hasTag(head, 0, "RIFF") || !hasTag(head, 8, "WAVE")) {
        return attributes;
    }
    quint32 sampleRate = 0;
    quint32 byteRate = 0;
    quint32 bits = 0;
    std::optional<quint64> dataBytes;
    qsizetype offset = 12;
    while (offset + 8 <= head.size()) {
        const quint32 length = le32(head, offset + 4);
        const qsizetype body = offset + 8;
        if (hasTag(head, offset, "fmt ") && body + 16 <= head.size()) {
            sampleRate = le32(head, body + 4);
            byteRate = le32(head, body + 8);
            bits = le16(head, body + 14);
        } else if (hasTag(head, offset, "data")) {
            const auto bodyStart = static_cast<quint64>(body);
            const quint64 available = fileSize > bodyStart ? fileSize - bodyStart : 0;
            dataBytes = std::min<quint64>(length, available);
            break;
        }
        offset = body + static_cast<qsizetype>(length) + static_cast<qsizetype>(length & 1U); // chunks are padded
    }
    if (sampleRate == 0) {
        return attributes;
    }
    attributes.sampleRate = sampleRate;
    attributes.bitDepth = bits;
    if (dataBytes && byteRate > 0) {
        attributes.duration = rounded(static_cast<double>(*dataBytes) / byteRate);
    }
    return attributes;
}

FileAttributes probeAiff(QByteArrayView head)
{
    FileAttributes attributes;
    if (!hasTag(head, 0, "FORM") || !(hasTag(head, 8, "AIFF") || hasTag(head, 8, "AIFC"))) {
        return attributes;
    }
    qsizetype offset = 12;
    while (offset + 8 <= head.size()) {
        const quint32 length = be32(head, offset + 4);
        const qsizetype body = offset + 8;
        if (hasTag(head, offset, "COMM") && body + 18 <= head.size()) {
            // channels (2), frames (4), bits (2), sample rate as an 80-bit extended float (exponent 2, mantissa 8).
            const quint32 frames = be32(head, body + 2);
            const quint32 bits = be16(head, body + 6);
            const auto exponent = static_cast<int>(be16(head, body + 8) & 0x7FFFU);
            const auto mantissa = qFromBigEndian<quint64>(head.data() + body + 10);
            const double rate = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
            if (rate < 1 || rate > 1e6) {
                return attributes;
            }
            attributes.sampleRate = rounded(rate);
            attributes.bitDepth = bits;
            attributes.duration = rounded(frames / rate);
            return attributes;
        }
        offset = body + static_cast<qsizetype>(length) + static_cast<qsizetype>(length & 1U);
    }
    return attributes;
}

FileAttributes probeAudio(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto size = static_cast<quint64>(file.size());
    const quint64 tag = id3v2Size(file.read(kId3HeaderBytes));
    if (tag >= size || !file.seek(static_cast<qint64>(tag))) {
        return {};
    }
    const QByteArray head = file.read(kProbeBytes);
    if (head.startsWith("fLaC")) {
        return probeFlac(head);
    }
    if (head.startsWith("RIFF")) {
        return probeWav(head, size - tag);
    }
    if (head.startsWith("FORM")) {
        return probeAiff(head);
    }
    if (QFileInfo(path).suffix().compare(u"mp3", Qt::CaseInsensitive) != 0) {
        return {};
    }
    quint64 audio = size - tag;
    constexpr auto kTrailer = static_cast<quint64>(kId3v1Bytes);
    if (audio >= kTrailer && file.seek(static_cast<qint64>(size - kTrailer)) && file.read(3) == "TAG") {
        audio -= kTrailer;
    }
    return probeMp3(head, audio);
}

} // namespace selecta::devnet
