// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/PcmFormat.h"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr int kMaxChunks = 4096;
constexpr int kMaxChannels = 32;
constexpr int kMaxSampleRate = 768000;
constexpr quint32 kSizeUnknown = 0xFFFFFFFFU;

// KSDATAFORMAT_SUBTYPE_* GUIDs share these 14 bytes after the 2-byte format tag.
constexpr std::array<quint8, 14> kGuidTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                              0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

ParseResult readOrWait(SourceReader &reader, qint64 position, qint64 count, std::vector<quint8> &out,
                       const char16_t *what)
{
    switch (reader.read(position, count, out)) {
    case SourceReader::Status::Ok:
        return ParseResult::success();
    case SourceReader::Status::NotAvailable:
        if (reader.isComplete()) {
            return ParseResult::failure(ParseStatus::Corrupt,
                                        u"file ends inside the %1"_s.arg(QString::fromUtf16(what)));
        }
        return ParseResult::needMoreData();
    case SourceReader::Status::Error:
        break;
    }
    return ParseResult::failure(ParseStatus::ReadError, reader.source().lastError().message);
}

// One chunk of a RIFF or IFF file: four-character id, size, where its body starts and where the next chunk starts.
struct Chunk
{
    std::array<quint8, 4> id{};
    quint32 size = 0;
    qint64 body = 0;
    qint64 next = 0;

    bool is(std::string_view tag) const { return fourcc(ByteSpan(id), 0, tag); }
};

ParseResult readChunk(SourceReader &reader, qint64 position, bool bigEndian, Chunk &chunk)
{
    std::vector<quint8> header;
    if (ParseResult r = readOrWait(reader, position, 8, header, u"chunk header"); !r.ok()) {
        return r;
    }
    std::copy_n(header.begin(), 4, chunk.id.begin());
    chunk.size = bigEndian ? be32(header, 4) : le32(header, 4);
    chunk.body = position + 8;
    chunk.next = chunk.body + static_cast<qint64>(chunk.size) + (chunk.size & 1U);
    return ParseResult::success();
}

// The common failure when a walk ends without the chunks it needs.
ParseResult missingChunks(const SourceReader &reader, qint64 position, const QString &what)
{
    if (!reader.isComplete() && position + 8 > reader.available()) {
        return ParseResult::needMoreData();
    }
    return ParseResult::failure(ParseStatus::Corrupt, what);
}

ParseResult headerBound(bool haveData, qint64 position, qint64 maxHeaderBytes)
{
    if (!haveData && position > maxHeaderBytes) {
        return ParseResult::failure(ParseStatus::TooLarge,
                                    u"no audio data in the first %1 bytes"_s.arg(maxHeaderBytes));
    }
    return ParseResult::success();
}

ParseResult validateRateAndChannels(qint64 rate, int channels)
{
    if (channels <= 0 || channels > kMaxChannels) {
        return ParseResult::failure(ParseStatus::Corrupt, u"invalid channel count %1"_s.arg(channels));
    }
    if (rate <= 0 || rate > kMaxSampleRate) {
        return ParseResult::failure(ParseStatus::Corrupt, u"invalid sample rate %1"_s.arg(rate));
    }
    return ParseResult::success();
}

// --- WAV --------------------------------------------------------------------------------------------------------

ParseResult wavEncoding(quint32 tag, int bits, int validBits, int container, PcmLayout &out)
{
    if (tag == 1) {
        if (container < 1 || container > 4 || bits > container * 8 || bits <= 0) {
            return ParseResult::failure(ParseStatus::Unsupported,
                                        u"%1-bit PCM in %2-byte containers"_s.arg(bits).arg(container));
        }
        out.encoding = container == 1 ? PcmEncoding::UnsignedInt : PcmEncoding::SignedInt;
        out.bitsPerSample = std::min(validBits, container * 8);
        return ParseResult::success();
    }
    if (tag == 3) {
        if (container != 4 && container != 8) {
            return ParseResult::failure(ParseStatus::Unsupported, u"%1-byte float samples"_s.arg(container));
        }
        out.encoding = PcmEncoding::Float;
        out.bitsPerSample = container * 8;
        return ParseResult::success();
    }
    return ParseResult::failure(ParseStatus::Unsupported, u"WAV format tag 0x%1"_s.arg(tag, 4, 16, u'0'));
}

ParseResult parseWavFormat(ByteSpan fmt, PcmLayout &layout)
{
    if (fmt.size() < 16) {
        return ParseResult::failure(ParseStatus::Corrupt, u"fmt chunk shorter than 16 bytes"_s);
    }
    quint32 tag = le16(fmt, 0);
    const int channels = static_cast<int>(le16(fmt, 2));
    const auto rate = static_cast<qint64>(le32(fmt, 4));
    const int blockAlign = static_cast<int>(le16(fmt, 12));
    const int bits = static_cast<int>(le16(fmt, 14));
    int validBits = bits;
    quint32 mask = 0;
    if (tag == 0xFFFE) {
        if (fmt.size() < 40 || le16(fmt, 16) < 22) {
            return ParseResult::failure(ParseStatus::Corrupt, u"WAVE_FORMAT_EXTENSIBLE fmt chunk too short"_s);
        }
        const int declaredValid = static_cast<int>(le16(fmt, 18));
        validBits = declaredValid > 0 ? declaredValid : bits;
        mask = le32(fmt, 20);
        tag = le16(fmt, 24);
        if (!std::equal(kGuidTail.begin(), kGuidTail.end(), fmt.begin() + 26)) {
            return ParseResult::failure(ParseStatus::Unsupported, u"unknown WAVE_FORMAT_EXTENSIBLE sub-format"_s);
        }
    }
    if (ParseResult r = validateRateAndChannels(rate, channels); !r.ok()) {
        return r;
    }
    if (blockAlign <= 0 || blockAlign % channels != 0) {
        return ParseResult::failure(ParseStatus::Corrupt, u"invalid block alignment %1"_s.arg(blockAlign));
    }
    PcmLayout out;
    out.channels = channels;
    out.sampleRate = static_cast<int>(rate);
    out.containerBytes = blockAlign / channels;
    out.channelMask = mask;
    out.bigEndian = false;
    if (ParseResult r = wavEncoding(tag, bits, validBits, out.containerBytes, out); !r.ok()) {
        return r;
    }
    layout = out;
    return ParseResult::success();
}

struct WavWalk
{
    bool haveFormat = false;
    PcmLayout format;
    qint64 dataOffset = -1;
    qint64 dataSize = -1;
    bool truncated = false;
};

// Handles one chunk; `done` is set once both `fmt ` and `data` are known.
ParseResult wavChunk(SourceReader &reader, const Chunk &chunk, qint64 end, WavWalk &walk, bool &done)
{
    if (chunk.is("fmt ")) {
        std::vector<quint8> fmt;
        if (ParseResult r = readOrWait(reader, chunk.body, std::min<qint64>(chunk.size, 64), fmt, u"fmt chunk");
            !r.ok()) {
            return r;
        }
        if (ParseResult r = parseWavFormat(fmt, walk.format); !r.ok()) {
            return r;
        }
        walk.haveFormat = true;
        done = walk.dataOffset >= 0;
        return ParseResult::success();
    }
    if (!chunk.is("data")) {
        return ParseResult::success();
    }
    const bool sizeUnknown = chunk.size == 0 || chunk.size == kSizeUnknown;
    walk.dataOffset = chunk.body;
    walk.truncated = !sizeUnknown && chunk.body + chunk.size > end;
    walk.dataSize = (sizeUnknown || walk.truncated) ? end - chunk.body : chunk.size;
    if (walk.haveFormat) {
        done = true;
        return ParseResult::success();
    }
    if (!reader.isComplete()) {
        return ParseResult::failure(ParseStatus::NeedsCompleteFile, u"WAV data chunk before fmt"_s);
    }
    if (sizeUnknown) {
        return ParseResult::failure(ParseStatus::Corrupt, u"WAV data chunk of unknown size before fmt"_s);
    }
    return ParseResult::success();
}

// --- AIFF -------------------------------------------------------------------------------------------------------

struct AiffEncoding
{
    PcmEncoding encoding = PcmEncoding::SignedInt;
    bool bigEndian = true;
    int floatBytes = 0; // 4 or 8 for float types
    int fixedBits = 0;  // in24/in32/raw force the word length
};

bool aiffCompression(ByteSpan type, AiffEncoding &out)
{
    if (fourcc(type, 0, "NONE") || fourcc(type, 0, "twos")) {
        out = {};
    } else if (fourcc(type, 0, "sowt")) {
        out = {PcmEncoding::SignedInt, false, 0, 0};
    } else if (fourcc(type, 0, "fl32") || fourcc(type, 0, "FL32")) {
        out = {PcmEncoding::Float, true, 4, 0};
    } else if (fourcc(type, 0, "fl64") || fourcc(type, 0, "FL64")) {
        out = {PcmEncoding::Float, true, 8, 0};
    } else if (fourcc(type, 0, "in24")) {
        out = {PcmEncoding::SignedInt, true, 0, 24};
    } else if (fourcc(type, 0, "in32")) {
        out = {PcmEncoding::SignedInt, true, 0, 32};
    } else if (fourcc(type, 0, "raw ")) {
        out = {PcmEncoding::UnsignedInt, true, 0, 8};
    } else {
        return false;
    }
    return true;
}

ParseResult parseComm(ByteSpan comm, bool aifc, PcmLayout &format, qint64 &frames)
{
    const int channels = static_cast<int>(be16(comm, 0));
    frames = be32(comm, 2);
    const int bits = static_cast<int>(be16(comm, 6));
    const double rate = readExtended80(comm.subspan(8, 10));
    AiffEncoding encoding;
    if (aifc && !aiffCompression(comm.subspan(18, 4), encoding)) {
        return ParseResult::failure(ParseStatus::Unsupported, u"AIFF-C compression type"_s);
    }
    // Range-check before converting: a crafted exponent can make the value huge.
    if (!std::isfinite(rate) || rate < 1.0 || rate > kMaxSampleRate || std::floor(rate) != rate) {
        return ParseResult::failure(ParseStatus::Corrupt, u"invalid sample rate %1"_s.arg(rate));
    }
    if (ParseResult r = validateRateAndChannels(static_cast<qint64>(rate), channels); !r.ok()) {
        return r;
    }
    format.channels = channels;
    format.sampleRate = static_cast<int>(rate);
    format.encoding = encoding.encoding;
    format.bigEndian = encoding.bigEndian;
    if (encoding.encoding == PcmEncoding::Float) {
        format.containerBytes = encoding.floatBytes;
        format.bitsPerSample = encoding.floatBytes * 8;
        return ParseResult::success();
    }
    const int wordBits = encoding.fixedBits > 0 ? encoding.fixedBits : bits;
    if (wordBits < 1 || wordBits > 32) {
        return ParseResult::failure(ParseStatus::Unsupported, u"%1-bit AIFF samples"_s.arg(wordBits));
    }
    format.containerBytes = (wordBits + 7) / 8;
    format.bitsPerSample = wordBits;
    if (!aifc && format.containerBytes == 1) {
        format.encoding = PcmEncoding::SignedInt; // AIFF 8-bit is signed
    }
    return ParseResult::success();
}

struct AiffWalk
{
    bool aifc = false;
    bool haveComm = false;
    PcmLayout format;
    qint64 frames = 0;
    qint64 dataOffset = -1;
    qint64 dataSize = -1;
};

ParseResult aiffChunk(SourceReader &reader, const Chunk &chunk, qint64 end, AiffWalk &walk, bool &done)
{
    if (chunk.is("COMM")) {
        if (chunk.size < (walk.aifc ? 22U : 18U)) {
            return ParseResult::failure(ParseStatus::Corrupt, u"COMM chunk too short"_s);
        }
        std::vector<quint8> comm;
        if (ParseResult r = readOrWait(reader, chunk.body, std::min<qint64>(chunk.size, 64), comm, u"COMM chunk");
            !r.ok()) {
            return r;
        }
        if (ParseResult r = parseComm(comm, walk.aifc, walk.format, walk.frames); !r.ok()) {
            return r;
        }
        walk.haveComm = true;
        done = walk.dataOffset >= 0;
        return ParseResult::success();
    }
    if (!chunk.is("SSND")) {
        return ParseResult::success();
    }
    if (chunk.size < 8) {
        return ParseResult::failure(ParseStatus::Corrupt, u"SSND chunk too short"_s);
    }
    std::vector<quint8> ssnd;
    if (ParseResult r = readOrWait(reader, chunk.body, 8, ssnd, u"SSND chunk"); !r.ok()) {
        return r;
    }
    const quint32 offset = be32(ssnd, 0);
    if (offset > chunk.size - 8) {
        return ParseResult::failure(ParseStatus::Corrupt, u"SSND offset beyond the chunk"_s);
    }
    walk.dataOffset = chunk.body + 8 + offset;
    const qint64 declared = static_cast<qint64>(chunk.size) - 8 - offset;
    walk.format.truncated = declared > end - walk.dataOffset;
    walk.dataSize = std::min(declared, end - walk.dataOffset);
    if (walk.haveComm) {
        done = true;
        return ParseResult::success();
    }
    if (!reader.isComplete()) {
        return ParseResult::failure(ParseStatus::NeedsCompleteFile, u"AIFF with COMM after SSND"_s);
    }
    return ParseResult::success();
}

} // namespace

double readExtended80(ByteSpan bytes)
{
    if (bytes.size() < 10 || (bytes[0] & 0x80U) != 0) {
        return 0.0;
    }
    const int exponent = static_cast<int>(((bytes[0] & 0x7FU) << 8U) | bytes[1]);
    const quint64 mantissa = be64(bytes, 2);
    if (mantissa == 0 || exponent == 0x7FFF) {
        return 0.0;
    }
    return std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
}

ParseResult parseWav(SourceReader &reader, qint64 maxHeaderBytes, PcmLayout &layout)
{
    std::vector<quint8> buffer;
    if (ParseResult r = readOrWait(reader, 0, 12, buffer, u"RIFF header"); !r.ok()) {
        return r;
    }
    if (fourcc(buffer, 0, "RF64") || fourcc(buffer, 0, "BW64") || fourcc(buffer, 0, "RIFX")) {
        return ParseResult::failure(ParseStatus::Unsupported, u"RF64, BW64 and RIFX files"_s);
    }
    if (!fourcc(buffer, 0, "RIFF") || !fourcc(buffer, 8, "WAVE")) {
        return ParseResult::failure(ParseStatus::Corrupt, u"not a RIFF/WAVE file"_s);
    }
    const qint64 end = reader.endOffset();
    WavWalk walk;
    qint64 pos = 12;
    bool done = false;
    for (int count = 0; count < kMaxChunks && !done && pos + 8 <= end; ++count) {
        if (ParseResult r = headerBound(walk.dataOffset >= 0, pos, maxHeaderBytes); !r.ok()) {
            return r;
        }
        Chunk chunk;
        if (ParseResult r = readChunk(reader, pos, false, chunk); !r.ok()) {
            return r;
        }
        if (ParseResult r = wavChunk(reader, chunk, end, walk, done); !r.ok()) {
            return r;
        }
        pos = chunk.next;
    }
    if (!walk.haveFormat || walk.dataOffset < 0) {
        return missingChunks(reader, pos, walk.haveFormat ? u"no data chunk"_s : u"no fmt chunk"_s);
    }
    walk.format.dataOffset = walk.dataOffset;
    walk.format.dataBytes = std::max<qint64>(0, walk.dataSize) / walk.format.blockAlign() * walk.format.blockAlign();
    walk.format.truncated = walk.truncated;
    layout = walk.format;
    return ParseResult::success();
}

ParseResult parseAiff(SourceReader &reader, qint64 maxHeaderBytes, PcmLayout &layout)
{
    std::vector<quint8> buffer;
    if (ParseResult r = readOrWait(reader, 0, 12, buffer, u"FORM header"); !r.ok()) {
        return r;
    }
    AiffWalk walk;
    walk.aifc = fourcc(buffer, 8, "AIFC");
    if (!fourcc(buffer, 0, "FORM") || (!walk.aifc && !fourcc(buffer, 8, "AIFF"))) {
        return ParseResult::failure(ParseStatus::Corrupt, u"not a FORM/AIFF file"_s);
    }
    const qint64 end = reader.endOffset();
    qint64 pos = 12;
    bool done = false;
    for (int count = 0; count < kMaxChunks && !done && pos + 8 <= end; ++count) {
        if (ParseResult r = headerBound(walk.dataOffset >= 0, pos, maxHeaderBytes); !r.ok()) {
            return r;
        }
        Chunk chunk;
        if (ParseResult r = readChunk(reader, pos, true, chunk); !r.ok()) {
            return r;
        }
        if (ParseResult r = aiffChunk(reader, chunk, end, walk, done); !r.ok()) {
            return r;
        }
        pos = chunk.next;
    }
    if (!walk.haveComm || walk.dataOffset < 0) {
        return missingChunks(reader, pos, walk.haveComm ? u"no SSND chunk"_s : u"no COMM chunk"_s);
    }
    PcmLayout format = walk.format;
    format.dataOffset = walk.dataOffset;
    const qint64 wholeFrames = std::max<qint64>(0, walk.dataSize) / format.blockAlign();
    format.truncated = format.truncated || walk.frames > wholeFrames;
    format.dataBytes = std::min(walk.frames, wholeFrames) * format.blockAlign();
    layout = format;
    return ParseResult::success();
}

namespace {

float convertInteger(const quint8 *p, int size, bool big)
{
    switch (size) {
    case 1: {
        const int v = static_cast<int>(p[0]) - (p[0] >= 128 ? 256 : 0);
        return static_cast<float>(v) / 128.0F;
    }
    case 2: {
        const auto v = static_cast<qint16>(big ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p));
        return static_cast<float>(v) / 32768.0F;
    }
    case 3: {
        const quint32 u = big ? ((quint32{p[0]} << 16U) | (quint32{p[1]} << 8U) | p[2])
                              : ((quint32{p[2]} << 16U) | (quint32{p[1]} << 8U) | p[0]);
        const qint32 v = static_cast<qint32>(u ^ 0x800000U) - 0x800000; // sign-extend 24 bits
        return static_cast<float>(v) / 8388608.0F;
    }
    default: {
        const auto v = static_cast<qint32>(big ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p));
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    }
}

float convertFloat(const quint8 *p, int size, bool big)
{
    float value = 0.0F;
    if (size == 4) {
        const quint32 bits = big ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
        value = std::bit_cast<float>(bits);
    } else {
        const quint64 bits = big ? qFromBigEndian<quint64>(p) : qFromLittleEndian<quint64>(p);
        value = static_cast<float>(std::bit_cast<double>(bits));
    }
    return std::isfinite(value) ? value : 0.0F;
}

} // namespace

void convertPcmToFloat(const quint8 *data, qint64 frames, const PcmLayout &layout, float *out)
{
    const qint64 count = frames * layout.channels;
    const int size = layout.containerBytes;
    const bool big = layout.bigEndian;
    for (qint64 i = 0; i < count; ++i) {
        const quint8 *p = data + (i * size);
        switch (layout.encoding) {
        case PcmEncoding::Float:
            out[i] = convertFloat(p, size, big);
            break;
        case PcmEncoding::UnsignedInt:
            out[i] = static_cast<float>(static_cast<int>(p[0]) - 128) / 128.0F;
            break;
        case PcmEncoding::SignedInt:
            out[i] = convertInteger(p, size, big);
            break;
        }
    }
}

} // namespace selecta::audio::detail
