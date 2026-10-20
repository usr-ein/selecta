// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/ContainerProbe.h"

#include "audio/detail/MpegAudio.h"

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr qint64 kMaxJunkBeforeMpeg = qint64{256} * 1024;
constexpr qint64 kSyncBlock = qint64{64} * 1024;
constexpr int kMpegChain = 3;
constexpr int kMaxTopLevelBoxes = 64;
constexpr qint64 kSyncError = -3;

ParseResult readError(SourceReader &reader)
{
    return ParseResult::failure(ParseStatus::ReadError, reader.source().lastError().message);
}

bool isMp4BoxType(ByteSpan b, std::size_t offset)
{
    return fourcc(b, offset, "ftyp") || fourcc(b, offset, "moov") || fourcc(b, offset, "mdat") ||
           fourcc(b, offset, "free") || fourcc(b, offset, "wide") || fourcc(b, offset, "skip");
}

// Size of the top-level box at `pos`, header included; 0 with a ParseResult on failure or missing bytes.
ParseResult mp4BoxSize(SourceReader &reader, qint64 pos, ByteSpan header, qint64 &size)
{
    const qint64 end = reader.endOffset();
    const quint32 small = be32(header, 0);
    qint64 headerBytes = 8;
    quint64 declared = small;
    if (small == 1) {
        std::vector<quint8> large;
        switch (reader.read(pos + 8, 8, large)) {
        case SourceReader::Status::Ok:
            break;
        case SourceReader::Status::NotAvailable:
            return reader.isComplete() ? ParseResult::failure(ParseStatus::Corrupt, u"truncated MP4 box"_s)
                                       : ParseResult::needMoreData();
        case SourceReader::Status::Error:
            return readError(reader);
        }
        declared = be64(large, 0);
        headerBytes = 16;
    } else if (small == 0) {
        declared = static_cast<quint64>(end - pos);
    }
    // A box may run up to 16 bytes past the announced size (sloppy writers); anything more is corrupt.
    const quint64 room = static_cast<quint64>(end - pos) + 16;
    if (std::cmp_less(declared, headerBytes) || declared > room) {
        return ParseResult::failure(ParseStatus::Corrupt, u"invalid MP4 box size"_s);
    }
    size = static_cast<qint64>(declared);
    return ParseResult::success();
}

ParseResult walkMp4(SourceReader &reader, qint64 maxHeaderBytes, ProbeResult &result)
{
    const qint64 end = reader.endOffset();
    std::vector<quint8> header;
    qint64 pos = 0;
    for (int box = 0; box < kMaxTopLevelBoxes && pos + 8 <= end; ++box) {
        switch (reader.read(pos, 8, header)) {
        case SourceReader::Status::Ok:
            break;
        case SourceReader::Status::NotAvailable:
            return ParseResult::needMoreData();
        case SourceReader::Status::Error:
            return readError(reader);
        }
        qint64 size = 0;
        if (ParseResult r = mp4BoxSize(reader, pos, header, size); !r.ok()) {
            return r;
        }
        if (fourcc(header, 4, "moov")) {
            if (size > maxHeaderBytes) {
                return ParseResult::failure(
                    ParseStatus::TooLarge,
                    u"MP4 index of %1 bytes exceeds the %2-byte limit"_s.arg(size).arg(maxHeaderBytes));
            }
            result.mp4MoovEnd = pos + size;
            return ParseResult::success();
        }
        if (fourcc(header, 4, "mdat")) {
            result.mp4MoovEnd = -1;
            return ParseResult::success();
        }
        pos += size;
    }
    if (!reader.isComplete() && pos + 8 > reader.available()) {
        return ParseResult::needMoreData();
    }
    result.mp4MoovEnd = -1; // neither found: let FFmpeg judge the complete file
    return ParseResult::success();
}

enum class Chain { Valid, Invalid, NeedMore, Error };

// Do `chain` more frames of the same stream follow the frame at `at`?
Chain checkMpegChain(SourceReader &reader, qint64 at, const MpegHeader &first, int chain)
{
    const qint64 end = reader.endOffset();
    std::vector<quint8> word;
    qint64 next = at + first.frameBytes;
    for (int k = 0; k < chain; ++k) {
        if (next >= end - 3) {
            return Chain::Valid; // the frame reaches the end of the file: nothing more to check
        }
        switch (reader.read(next, 4, word)) {
        case SourceReader::Status::Ok:
            break;
        case SourceReader::Status::NotAvailable:
            return Chain::NeedMore;
        case SourceReader::Status::Error:
            return Chain::Error;
        }
        const std::optional<MpegHeader> h = parseMpegHeader(be32(word, 0));
        if (!h || h->frameBytes == 0 || !sameMpegStream(first, *h)) {
            return Chain::Invalid;
        }
        next += h->frameBytes;
    }
    return Chain::Valid;
}

// The first validated sync in `block` (which starts at `start`) before `scanEnd`; -1 none, -2 need more bytes,
// kSyncError on a read error.
qint64 scanForSync(SourceReader &reader, const std::vector<quint8> &block, qint64 start, qint64 scanEnd, int chain)
{
    for (qint64 i = 0; i < scanEnd; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (block[idx] != 0xFF || (block[idx + 1] & 0xE0U) != 0xE0) {
            continue;
        }
        const std::optional<MpegHeader> first = parseMpegHeader(be32(block, idx));
        if (!first || first->frameBytes == 0) {
            continue;
        }
        switch (checkMpegChain(reader, start + i, *first, chain)) {
        case Chain::Valid:
            return start + i;
        case Chain::NeedMore:
            return -2;
        case Chain::Error:
            return kSyncError;
        case Chain::Invalid:
            break;
        }
    }
    return -1;
}

} // namespace

qint64 findMpegSync(SourceReader &reader, qint64 from, qint64 maxScan, int chain)
{
    const qint64 end = reader.endOffset();
    const qint64 limit = std::min(from + maxScan, end);
    std::vector<quint8> block;
    for (qint64 start = from; start < limit; start += kSyncBlock) {
        const qint64 wanted = std::min(kSyncBlock + 3, end - start);
        const qint64 count = std::min(wanted, reader.available() - start);
        if (count < 4) {
            return reader.isComplete() ? -1 : -2;
        }
        if (reader.read(start, count, block) != SourceReader::Status::Ok) {
            return -1;
        }
        const qint64 found = scanForSync(reader, block, start, std::min<qint64>(count - 3, limit - start), chain);
        if (found == kSyncError) {
            return -1;
        }
        if (found != -1) {
            return found;
        }
        if (count < wanted) {
            return reader.isComplete() ? -1 : -2;
        }
    }
    return -1;
}

ParseResult probeContainer(SourceReader &reader, qint64 maxHeaderBytes, ProbeResult &result)
{
    result = {};
    const qint64 end = reader.endOffset();
    std::vector<quint8> bytes;

    // Leading ID3v2 tags (possibly several), skipped by size without reading them.
    qint64 pos = 0;
    while (pos + 10 <= end) {
        const SourceReader::Status s = reader.read(pos, 10, bytes);
        if (s == SourceReader::Status::NotAvailable) {
            return ParseResult::needMoreData();
        }
        if (s == SourceReader::Status::Error) {
            return readError(reader);
        }
        const qint64 tag = id3v2TagSize(bytes);
        if (tag <= 0) {
            break; // no tag, or an invalid one: treated as junk before the stream
        }
        pos += tag;
        if (pos > maxHeaderBytes) {
            return ParseResult::failure(
                ParseStatus::TooLarge,
                u"ID3v2 tags of %1 bytes exceed the %2-byte limit"_s.arg(pos).arg(maxHeaderBytes));
        }
    }
    result.leadingTagBytes = pos;

    const qint64 headBytes = std::min<qint64>(12, end - pos);
    if (headBytes < 4) {
        return reader.isComplete() ? ParseResult::failure(ParseStatus::Corrupt, u"file too short"_s)
                                   : ParseResult::needMoreData();
    }
    switch (reader.read(pos, headBytes, bytes)) {
    case SourceReader::Status::Ok:
        break;
    case SourceReader::Status::NotAvailable:
        return ParseResult::needMoreData();
    case SourceReader::Status::Error:
        return readError(reader);
    }
    const ByteSpan head(bytes);
    if (fourcc(head, 0, "fLaC")) {
        result.container = Container::Flac;
        result.streamStart = pos;
        return ParseResult::success();
    }
    if (pos == 0 && headBytes >= 12) {
        if ((fourcc(head, 0, "RIFF") || fourcc(head, 0, "RF64") || fourcc(head, 0, "BW64") ||
             fourcc(head, 0, "RIFX")) &&
            fourcc(head, 8, "WAVE")) {
            result.container = Container::Wav;
            return ParseResult::success();
        }
        if (fourcc(head, 0, "FORM") && (fourcc(head, 8, "AIFF") || fourcc(head, 8, "AIFC"))) {
            result.container = Container::Aiff;
            return ParseResult::success();
        }
        if (isMp4BoxType(head, 4)) {
            result.container = Container::Mp4;
            return walkMp4(reader, maxHeaderBytes, result);
        }
    }
    if (pos == 0 && fourcc(head, 0, "OggS")) {
        result.container = Container::Ogg;
        return ParseResult::success();
    }
    const qint64 sync = findMpegSync(reader, pos, kMaxJunkBeforeMpeg, kMpegChain);
    if (sync >= 0) {
        result.container = Container::MpegAudio;
        result.streamStart = sync;
        return ParseResult::success();
    }
    if (sync == -2) {
        return ParseResult::needMoreData();
    }
    result.container = Container::Unknown;
    return ParseResult::success();
}

} // namespace selecta::audio::detail
