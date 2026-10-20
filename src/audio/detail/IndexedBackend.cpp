// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/IndexedBackend.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr qint64 kKiB = 1024;
constexpr qint64 kCacheBlock = 256 * kKiB;
constexpr qint64 kScanBlock = 64 * kKiB; // scanned at a time; the cache window keeps a margin after it
constexpr qint64 kMaxResyncBytes = 1024 * kKiB;
constexpr qint64 kChunkTargetFrames = 8192;
constexpr qint64 kPcmChunkFrames = 8192;
constexpr qint64 kMaxFlacFrameBytes = kKiB * 1024 * 16;
constexpr int kMpegResyncChain = 2;
constexpr int kMaxFlacMetadataBlocks = 1024;
constexpr qint64 kFlacFirstFrameSearch = 64 * kKiB;

QString pcmCodecName(const PcmLayout &layout)
{
    QString name = u"pcm_"_s;
    if (layout.encoding == PcmEncoding::Float) {
        name += u'f';
    } else if (layout.encoding == PcmEncoding::UnsignedInt) {
        name += u'u';
    } else {
        name += u's';
    }
    name += QString::number(layout.containerBytes * 8);
    if (layout.containerBytes > 1) {
        name += layout.bigEndian ? u"be"_s : u"le"_s;
    }
    return name;
}

bool flacNumberFollows(const FlacFrameHeader &current, const FlacFrameHeader &next)
{
    if (current.variableBlockSize != next.variableBlockSize) {
        return false;
    }
    return current.variableBlockSize ? next.codedNumber == current.codedNumber + current.blockSize
                                     : next.codedNumber == current.codedNumber + 1;
}

// Bytes of tags at the end of a complete file (ID3v1, APEv2) that aren't audio.
qint64 trailingTagBytes(SourceReader &reader)
{
    const qint64 end = reader.endOffset();
    qint64 trailer = 0;
    std::vector<quint8> bytes;
    if (end >= 128 && reader.read(end - 128, 3, bytes) == SourceReader::Status::Ok && bytes[0] == 'T' &&
        bytes[1] == 'A' && bytes[2] == 'G') {
        trailer = 128;
    }
    if (end - trailer >= 32 && reader.read(end - trailer - 32, 32, bytes) == SourceReader::Status::Ok &&
        fourcc(bytes, 0, "APET") && fourcc(bytes, 4, "AGEX")) {
        const qint64 size = le32(bytes, 12); // items + footer
        const bool hasHeader = (le32(bytes, 20) & 0x80000000U) != 0;
        trailer += size + (hasHeader ? 32 : 0);
    }
    return std::clamp<qint64>(trailer, 0, end);
}

} // namespace

std::size_t FrameIndex::frameFor(qint64 sample) const
{
    const auto it = std::ranges::upper_bound(frames, sample, std::less<>{}, &IndexedFrame::firstSample);
    if (it == frames.begin()) {
        return 0; // before the first frame, or no frames
    }
    const auto i = static_cast<std::size_t>(std::distance(frames.begin(), it)) - 1;
    if (sample >= frames[i].firstSample + frames[i].samples) {
        return frames.size();
    }
    return i;
}

const quint8 *IndexedBackend::BlockCache::peek(SourceReader &reader, qint64 position, qint64 count)
{
    if (position >= m_start && count <= m_end - position) {
        return m_data.data() + (position - m_start);
    }
    if (position < 0 || count < 0 || position > reader.available() || count > reader.available() - position) {
        return nullptr;
    }
    const qint64 length = std::min(std::max(count, kCacheBlock), reader.available() - position);
    const SourceReader::Status status = reader.read(position, length, m_data);
    if (status != SourceReader::Status::Ok) {
        failed = status == SourceReader::Status::Error;
        m_start = m_end = 0;
        return nullptr;
    }
    m_start = position;
    m_end = position + length;
    return m_data.data();
}

IndexedBackend::IndexedBackend(Kind kind, std::shared_ptr<ByteSource> source, const ProbeResult &probe,
                               const DecoderOptions &options)
    : m_kind(kind)
    , m_source(std::move(source))
    , m_probe(probe)
    , m_options(options)
{}

IndexedBackend::~IndexedBackend() = default;

int IndexedBackend::prerollFrames(Kind kind)
{
    // MP3: nine frames drain the bit reservoir and settle the filterbank (Mixxx, audio §5.2). FLAC and PCM frames are
    // independent.
    return kind == Kind::Mpeg ? 9 : 0;
}

Backend::Status IndexedBackend::open(Error &error)
{
    const SourceSnapshot snapshot = m_source->snapshot();
    SourceReader reader(*m_source, snapshot);
    m_cache.clear();
    m_cache.failed = false;
    Status status = Status::Error;
    switch (m_kind) {
    case Kind::Mpeg:
        status = openMpeg(reader, error);
        break;
    case Kind::Flac:
        status = openFlac(reader, error);
        break;
    case Kind::Pcm:
        status = openPcm(reader, error);
        break;
    }
    if (status == Status::NeedMoreData && snapshot.state == SourceState::Failed) {
        error = makeError(ErrorCode::SourceFailed, u"the download stopped before the audio header arrived"_s);
        return Status::Error;
    }
    if (status != Status::Ok) {
        return status;
    }
    if (snapshot.isComplete() && m_kind != Kind::Pcm && !m_indexShared) {
        // Index the whole file now: exact length and statistics, and an index clones can share.
        while (true) {
            const Extend extend = extendIndex(error);
            if (extend == Extend::Failed) {
                return Status::Error;
            }
            if (extend != Extend::Progress) {
                break;
            }
        }
    }
    finishInfo();
    return Status::Ok;
}

Backend::Status IndexedBackend::openMpeg(SourceReader &reader, Error &error)
{
    if (!m_indexShared) {
        m_index = std::make_shared<FrameIndex>();
    }
    const qint64 start = m_probe.streamStart;
    const quint8 *bytes = m_cache.peek(reader, start, 4);
    if (bytes == nullptr) {
        if (m_cache.failed) {
            error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
            return Status::Error;
        }
        return Status::NeedMoreData;
    }
    const std::optional<MpegHeader> header = parseMpegHeader(be32(ByteSpan(bytes, 4), 0));
    if (!header) {
        error = makeError(ErrorCode::Corrupt, u"no MPEG audio frame where one was found"_s);
        return Status::Error;
    }
    if (header->frameBytes == 0) {
        error = makeError(ErrorCode::Unsupported, u"free-format MPEG audio"_s);
        return Status::Error;
    }
    m_firstHeader = *header;
    m_firstAudioOffset = start;
    const qint64 frameEnd = start + header->frameBytes;
    if (frameEnd <= reader.endOffset()) {
        bytes = m_cache.peek(reader, start, header->frameBytes);
        if (bytes == nullptr) {
            if (m_cache.failed) {
                error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
                return Status::Error;
            }
            return Status::NeedMoreData;
        }
        const ByteSpan frame(bytes, static_cast<std::size_t>(header->frameBytes));
        m_lame = parseXingFrame(frame, *header);
        qint64 vbriFrames = 0;
        const bool vbri = parseVbriFrame(frame, &vbriFrames);
        if (m_lame || vbri) {
            m_firstAudioOffset = frameEnd; // the tag frame carries no audio
        }
        if (m_lame && m_lame->frames > 0) {
            const qint64 declared = m_lame->frames * header->samplesPerFrame;
            if (declared > m_options.maxDurationSeconds * header->sampleRate) {
                qCDebug(lcAudio) << "ignoring an implausible Xing frame count" << m_lame->frames;
                m_lame->frames = -1;
            }
        }
    }
    m_scanPos = m_firstAudioOffset;

    AVCodecID codec = AV_CODEC_ID_MP3;
    m_info.codec = Codec::Mp3;
    if (header->layer == 2) {
        codec = AV_CODEC_ID_MP2;
        m_info.codec = Codec::Mp2;
    } else if (header->layer == 1) {
        codec = AV_CODEC_ID_MP1;
        m_info.codec = Codec::Mp1;
    }
    // FFmpeg never trims MPEG audio here: the Decoder applies the LAME tag's delay and padding itself.
    if (const Result<void> opened = m_codec.open(codec, header->sampleRate, header->channels, {}, true); !opened) {
        error = opened.error();
        return Status::Error;
    }
    m_info.container = Container::MpegAudio;
    m_info.codecName = m_codec.codecName();
    m_info.sampleRate = header->sampleRate;
    m_info.channels = header->channels;
    m_info.lameTag = m_lame;
    return Status::Ok;
}

Backend::Status IndexedBackend::unavailable(const SourceReader &reader, Error &error, const QString &cutShort) const
{
    if (m_cache.failed) {
        error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
        return Status::Error;
    }
    if (reader.isComplete()) {
        error = makeError(ErrorCode::Corrupt, cutShort);
        return Status::Error;
    }
    return Status::NeedMoreData;
}

Backend::Status IndexedBackend::readFlacMetadata(SourceReader &reader, qint64 &audioStart, Error &error)
{
    const qint64 start = m_probe.streamStart;
    qint64 pos = start + 4;
    bool last = false;
    bool haveInfo = false;
    for (int block = 0; !last && block < kMaxFlacMetadataBlocks; ++block) {
        const quint8 *header = m_cache.peek(reader, pos, 4);
        if (header == nullptr) {
            return unavailable(reader, error, u"FLAC metadata runs past the end of the file"_s);
        }
        last = (header[0] & 0x80U) != 0;
        const unsigned type = header[0] & 0x7FU;
        const qint64 length = be24(ByteSpan(header, 4), 1);
        if (type == 127 || (block == 0 && type != 0) || (type == 0 && length < 34)) {
            error = makeError(ErrorCode::Corrupt, u"invalid FLAC metadata block"_s);
            return Status::Error;
        }
        if (type == 0) {
            const quint8 *body = m_cache.peek(reader, pos + 4, 34);
            if (body == nullptr) {
                return unavailable(reader, error, u"FLAC STREAMINFO is cut short"_s);
            }
            const std::optional<FlacStreamInfo> info = parseFlacStreamInfo(ByteSpan(body, 34));
            if (!info) {
                error = makeError(ErrorCode::Corrupt, u"invalid FLAC STREAMINFO"_s);
                return Status::Error;
            }
            m_flac = *info;
            haveInfo = true;
        }
        pos += 4 + length;
        if (pos - start > m_options.maxHeaderBytes) {
            error =
                makeError(ErrorCode::TooLarge, u"FLAC metadata of more than %1 bytes"_s.arg(m_options.maxHeaderBytes));
            return Status::Error;
        }
    }
    if (!haveInfo || !last) {
        error = makeError(ErrorCode::Corrupt, u"FLAC metadata without STREAMINFO or end"_s);
        return Status::Error;
    }
    audioStart = pos;
    return Status::Ok;
}

Backend::Status IndexedBackend::findFirstFlacFrame(SourceReader &reader, qint64 from, Error &error)
{
    // The first frame header, or the first valid one shortly after (some tools leave junk after the metadata).
    const qint64 end = reader.endOffset();
    const qint64 limit = std::min(end, from + kFlacFirstFrameSearch);
    for (qint64 at = from; at < limit; ++at) {
        const qint64 count = std::min<qint64>(kFlacMaxFrameHeaderBytes, end - at);
        const quint8 *bytes = m_cache.peek(reader, at, count);
        if (bytes == nullptr) {
            return unavailable(reader, error, u"FLAC stream is cut short"_s);
        }
        const std::optional<FlacFrameHeader> header =
            parseFlacFrameHeader(ByteSpan(bytes, static_cast<std::size_t>(count)));
        if (header && flacHeaderMatches(*header, m_flac)) {
            if (!m_indexShared) {
                m_index->skippedBytes += at - from;
            }
            m_scanPos = at;
            m_fixedBlockSize = header->variableBlockSize ? 0 : header->blockSize;
            m_pendingHeader = header;
            return Status::Ok;
        }
    }
    error = makeError(ErrorCode::Corrupt, u"no FLAC frame after the metadata"_s);
    return Status::Error;
}

Backend::Status IndexedBackend::openFlac(SourceReader &reader, Error &error)
{
    if (!m_indexShared) {
        m_index = std::make_shared<FrameIndex>();
    }
    qint64 audioStart = 0;
    if (const Status status = readFlacMetadata(reader, audioStart, error); status != Status::Ok) {
        return status;
    }
    m_firstAudioOffset = audioStart;
    m_scanPos = audioStart;
    if (audioStart < reader.endOffset()) {
        if (const Status status = findFirstFlacFrame(reader, audioStart, error); status != Status::Ok) {
            return status;
        }
    }
    const qint64 uncompressedBound =
        (static_cast<qint64>(m_flac.maxBlockSize) * m_flac.channels * ((m_flac.bitsPerSample + 7) / 8)) + 4096;
    m_maxFrameBytes =
        std::min(kMaxFlacFrameBytes, std::max<qint64>(uncompressedBound, qint64{m_flac.maxFrameSize} * 2));
    if (const Result<void> opened =
            m_codec.open(AV_CODEC_ID_FLAC, m_flac.sampleRate, m_flac.channels, m_flac.raw, false);
        !opened) {
        error = opened.error();
        return Status::Error;
    }
    m_info.container = Container::Flac;
    m_info.codec = Codec::Flac;
    m_info.codecName = m_codec.codecName();
    m_info.sampleRate = m_flac.sampleRate;
    m_info.channels = m_flac.channels;
    m_info.bitsPerSample = m_flac.bitsPerSample;
    m_info.lossless = true;
    return Status::Ok;
}

Backend::Status IndexedBackend::openPcm(SourceReader &reader, Error &error)
{
    const ParseResult parsed = m_probe.container == Container::Aiff ? parseAiff(reader, m_options.maxHeaderBytes, m_pcm)
                                                                    : parseWav(reader, m_options.maxHeaderBytes, m_pcm);
    if (parsed.status == ParseStatus::NeedMoreData) {
        return Status::NeedMoreData;
    }
    if (!parsed.ok()) {
        error = makeError(errorCodeFor(parsed.status), parsed.message);
        return Status::Error;
    }
    m_info.container = m_probe.container;
    m_info.codec = Codec::Pcm;
    m_info.codecName = pcmCodecName(m_pcm);
    m_info.sampleRate = m_pcm.sampleRate;
    m_info.channels = m_pcm.channels;
    m_info.bitsPerSample = m_pcm.bitsPerSample;
    m_info.floatingPoint = m_pcm.encoding == PcmEncoding::Float;
    m_info.lossless = true;
    m_nextPcmFrame = 0;
    return Status::Ok;
}

IndexedBackend::Extend IndexedBackend::extendIndex(Error &error)
{
    if (m_kind == Kind::Pcm || m_indexShared || !m_index || m_index->finished) {
        return Extend::Done;
    }
    SourceReader reader(*m_source, m_source->snapshot());
    const Extend result = m_kind == Kind::Mpeg ? extendMpeg(reader, error) : extendFlac(reader, error);
    if (result == Extend::Progress || result == Extend::Done) {
        const qint64 maxSamples = m_options.maxDurationSeconds * m_info.sampleRate;
        if (m_index->totalSamples > maxSamples) {
            error = makeError(ErrorCode::TooLarge,
                              u"stream longer than %1 hours"_s.arg(m_options.maxDurationSeconds / 3600));
            return Extend::Failed;
        }
    }
    return result;
}

void IndexedBackend::appendMpegFrame(const MpegHeader &header)
{
    FrameIndex &index = *m_index;
    index.frames.push_back({m_scanPos, index.totalSamples, static_cast<quint32>(header.frameBytes),
                            static_cast<quint32>(header.samplesPerFrame)});
    index.totalSamples += header.samplesPerFrame;
    index.totalBytes += header.frameBytes;
    index.minBitrate = index.minBitrate == 0 ? header.bitrateKbps : std::min(index.minBitrate, header.bitrateKbps);
    index.maxBitrate = std::max(index.maxBitrate, header.bitrateKbps);
    m_scanPos += header.frameBytes;
}

IndexedBackend::Skip IndexedBackend::skipNonFrame(SourceReader &reader, ByteSpan head, qint64 end)
{
    FrameIndex &index = *m_index;
    const bool id3v1 = head[0] == 'T' && head[1] == 'A' && head[2] == 'G' && end - m_scanPos <= 128 + 256;
    if (id3v1 || fourcc(head, 0, "APET") || fourcc(head, 0, "LYRI")) {
        index.finished = true; // a trailing tag (ID3v1, Lyrics3, APEv2): the audio is over
        return Skip::Finished;
    }
    if (head[0] == 'I' && head[1] == 'D' && head[2] == '3') {
        const quint8 *tag = m_cache.peek(reader, m_scanPos, 10);
        if (tag == nullptr) {
            return Skip::Blocked;
        }
        const qint64 size = id3v2TagSize(ByteSpan(tag, 10));
        if (size > 0) {
            index.skippedBytes += size; // an ID3v2 tag inside the stream (concatenated files)
            m_scanPos += size;
            return Skip::Continue;
        }
    }
    const qint64 sync = findMpegSync(reader, m_scanPos + 1, kMaxResyncBytes, kMpegResyncChain);
    if (sync == -2) {
        return Skip::Blocked;
    }
    if (sync < 0) {
        index.skippedBytes += end - m_scanPos;
        index.finished = true;
        return Skip::Finished;
    }
    index.skippedBytes += sync - m_scanPos;
    m_scanPos = sync;
    return Skip::Continue;
}

IndexedBackend::Extend IndexedBackend::extendMpeg(SourceReader &reader, Error &error)
{
    FrameIndex &index = *m_index;
    const qint64 end = reader.endOffset();
    const qint64 available = reader.available();
    const std::size_t before = index.frames.size();
    const auto blocked = [&] {
        if (m_cache.failed) {
            error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
            return Extend::Failed;
        }
        return index.frames.size() > before ? Extend::Progress : Extend::Blocked;
    };
    while (!index.finished) {
        if (m_scanPos + 4 > end) {
            index.finished = true;
            break;
        }
        const quint8 *bytes = m_cache.peek(reader, m_scanPos, 4);
        if (bytes == nullptr) {
            return blocked();
        }
        const ByteSpan head(bytes, 4);
        const std::optional<MpegHeader> header = parseMpegHeader(be32(head, 0));
        if (header && header->frameBytes > 0 && sameMpegStream(*header, m_firstHeader)) {
            const qint64 frameEnd = m_scanPos + header->frameBytes;
            if (frameEnd > end) {
                index.truncated = true;
                index.finished = true;
                break;
            }
            if (frameEnd > available) {
                return blocked();
            }
            appendMpegFrame(*header);
            continue;
        }
        if (skipNonFrame(reader, head, end) == Skip::Blocked) {
            return blocked();
        }
    }
    return index.frames.size() > before ? Extend::Progress : Extend::Done;
}

void IndexedBackend::appendFlacFrame(const FlacFrameHeader &current, qint64 frameEnd)
{
    FrameIndex &index = *m_index;
    const qint64 first = flacFirstSample(current, m_fixedBlockSize);
    if (!index.frames.empty() && first != index.totalSamples) {
        // A gap in the numbering (frames lost to corruption): the timeline follows the stream's own numbers.
        qCDebug(lcAudio) << "FLAC frame numbering jumps from" << index.totalSamples << "to" << first;
    }
    const qint64 firstSample = index.frames.empty() ? first : std::max(first, index.totalSamples);
    index.frames.push_back(
        {m_scanPos, firstSample, static_cast<quint32>(frameEnd - m_scanPos), static_cast<quint32>(current.blockSize)});
    index.totalSamples = firstSample + current.blockSize;
    index.totalBytes += frameEnd - m_scanPos;
}

IndexedBackend::Candidate IndexedBackend::checkFlacCandidate(SourceReader &reader, qint64 at,
                                                             const FlacFrameHeader &current, qint64 limit,
                                                             std::optional<FlacFrameHeader> &next)
{
    const qint64 headerBytes = std::min<qint64>(kFlacMaxFrameHeaderBytes, reader.endOffset() - at);
    if (at + headerBytes > reader.available()) {
        return Candidate::Blocked; // the candidate's header hasn't fully arrived
    }
    const quint8 *bytes = m_cache.peek(reader, at, headerBytes);
    if (bytes == nullptr) {
        return Candidate::Blocked;
    }
    next = parseFlacFrameHeader(ByteSpan(bytes, static_cast<std::size_t>(headerBytes)));
    if (!next || !flacHeaderMatches(*next, m_flac)) {
        return Candidate::Reject;
    }
    if (flacNumberFollows(current, *next)) {
        return Candidate::Accept;
    }
    // Past the size bound the current frame is corrupt: any later frame resynchronises the stream.
    if (at >= limit && flacFirstSample(*next, m_fixedBlockSize) > flacFirstSample(current, m_fixedBlockSize)) {
        m_index->skippedBytes += at - limit;
        return Candidate::Accept;
    }
    return Candidate::Reject;
}

IndexedBackend::FlacSearch IndexedBackend::searchNextFlacFrame(SourceReader &reader, const FlacFrameHeader &current)
{
    FlacSearch search;
    const qint64 end = reader.endOffset();
    const qint64 available = reader.available();
    const qint64 limit = m_scanPos + m_maxFrameBytes;
    qint64 from = std::max(m_searchFrom, m_scanPos + current.headerBytes + 3);
    while (from + 1 < std::min(available, end)) {
        const qint64 blockEnd = std::min({available, end, from + kScanBlock});
        const qint64 count = blockEnd - from;
        const quint8 *block = m_cache.peek(reader, from, count);
        for (qint64 i = 0; block != nullptr && i + 1 < count; ++i) {
            // Jump to the next 0xFF: compressed audio rarely holds one, so memchr does most of the scanning.
            const void *hit = std::memchr(block + i, 0xFF, static_cast<std::size_t>(count - 1 - i));
            if (hit == nullptr) {
                break;
            }
            i = static_cast<const quint8 *>(hit) - block;
            if ((block[i + 1] & 0xFEU) != 0xF8) {
                continue;
            }
            switch (checkFlacCandidate(reader, from + i, current, limit, search.header)) {
            case Candidate::Accept:
                search.result = FlacSearch::Result::Found;
                search.at = from + i;
                return search;
            case Candidate::Blocked:
                m_searchFrom = from + i;
                search.result = FlacSearch::Result::Blocked;
                return search;
            case Candidate::Reject:
                break;
            }
            block = m_cache.peek(reader, from, count); // the check may have moved the cache window
        }
        if (block == nullptr) {
            search.result = FlacSearch::Result::Blocked;
            return search;
        }
        if (blockEnd >= available || blockEnd >= end) {
            break;
        }
        from = blockEnd - 1; // a sync code may straddle the block boundary
    }
    search.result = FlacSearch::Result::NotFound;
    return search;
}

IndexedBackend::Extend IndexedBackend::extendFlac(SourceReader &reader, Error &error)
{
    FrameIndex &index = *m_index;
    const std::size_t before = index.frames.size();
    while (!index.finished && m_pendingHeader) {
        const FlacFrameHeader current = *m_pendingHeader;
        const FlacSearch search = searchNextFlacFrame(reader, current);
        if (search.result == FlacSearch::Result::Found && search.header) {
            appendFlacFrame(current, search.at);
            m_scanPos = search.at;
            m_pendingHeader = search.header;
            m_searchFrom = 0;
            continue;
        }
        if (m_cache.failed) {
            error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
            return Extend::Failed;
        }
        if (search.result == FlacSearch::Result::NotFound &&
            (reader.isComplete() || reader.available() >= reader.endOffset())) {
            // The last frame runs to the end of the audio.
            const qint64 audioEnd = reader.endOffset() - trailingTagBytes(reader);
            if (audioEnd > m_scanPos + current.headerBytes) {
                appendFlacFrame(current, audioEnd);
            } else {
                index.truncated = true;
            }
            index.finished = true;
            break;
        }
        if (search.result == FlacSearch::Result::NotFound) {
            m_searchFrom = std::max(m_scanPos + current.headerBytes + 3, reader.available() - 1);
        }
        return index.frames.size() > before ? Extend::Progress : Extend::Blocked;
    }
    if (!m_pendingHeader) {
        index.finished = true; // no frames at all
    }
    return index.frames.size() > before ? Extend::Progress : Extend::Done;
}

Backend::Status IndexedBackend::outOfData(Error &error) const
{
    if (m_source->state() == SourceState::Failed) {
        error = makeError(ErrorCode::SourceFailed, u"the download stopped before the audio needed"_s);
        return Status::Error;
    }
    return Status::NeedMoreData;
}

Backend::Status IndexedBackend::decode(DecodedChunk &chunk, Error &error)
{
    return m_kind == Kind::Pcm ? decodePcm(chunk, error) : decodeFrames(chunk, error);
}

Backend::Status IndexedBackend::decodeFrames(DecodedChunk &chunk, Error &error)
{
    chunk.clear();
    chunk.channels = m_info.channels;
    qint64 expectedNext = -1;
    while (chunk.frames() < kChunkTargetFrames) {
        if (m_nextFrame >= m_index->frames.size()) {
            if (chunk.frames() > 0) {
                break;
            }
            const Extend extend = extendIndex(error);
            if (extend == Extend::Failed) {
                return Status::Error;
            }
            if (m_nextFrame < m_index->frames.size()) {
                continue;
            }
            if (m_index->finished) {
                return Status::EndOfStream;
            }
            return outOfData(error);
        }
        const IndexedFrame frame = m_index->frames[m_nextFrame];
        if (expectedNext >= 0 && frame.firstSample != expectedNext) {
            break; // keep each chunk contiguous on the timeline
        }
        SourceReader reader(*m_source, m_source->snapshot());
        const quint8 *bytes = m_cache.peek(reader, frame.offset, frame.size);
        if (bytes == nullptr) {
            // Indexed bytes are always available: only an I/O error gets here.
            error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
            return Status::Error;
        }
        if (chunk.frames() == 0) {
            chunk.firstSample = frame.firstSample;
        }
        const std::size_t before = chunk.samples.size();
        const std::size_t wanted = before + (static_cast<std::size_t>(frame.samples) * m_info.channels);
        const Result<qint64> decoded =
            m_codec.decode(bytes, static_cast<int>(frame.size), frame.firstSample, chunk.samples, nullptr);
        if (!decoded || std::cmp_not_equal(decoded.value(), frame.samples)) {
            // A rejected or odd frame becomes silence of its own length (or is cut to it), so the timeline holds.
            ++m_decodeErrors;
            if (!decoded) {
                chunk.samples.resize(before);
            }
            chunk.samples.resize(wanted, 0.0F);
        }
        expectedNext = frame.firstSample + frame.samples;
        ++m_nextFrame;
    }
    return Status::Ok;
}

qint64 IndexedBackend::pcmAvailableFrames(SourceSnapshot snapshot) const
{
    const qint64 dataEnd = m_pcm.dataOffset + m_pcm.dataBytes;
    const qint64 usable = std::min(snapshot.available, dataEnd) - m_pcm.dataOffset;
    return usable > 0 ? usable / m_pcm.blockAlign() : 0;
}

Backend::Status IndexedBackend::decodePcm(DecodedChunk &chunk, Error &error)
{
    chunk.clear();
    chunk.channels = m_pcm.channels;
    const qint64 total = m_pcm.frames();
    if (m_nextPcmFrame >= total) {
        return Status::EndOfStream;
    }
    const SourceSnapshot snapshot = m_source->snapshot();
    const qint64 ready = pcmAvailableFrames(snapshot);
    if (m_nextPcmFrame >= ready) {
        if (snapshot.isComplete()) {
            return Status::EndOfStream;
        }
        return outOfData(error);
    }
    const qint64 frames = std::min(kPcmChunkFrames, ready - m_nextPcmFrame);
    SourceReader reader(*m_source, snapshot);
    const qint64 offset = m_pcm.dataOffset + (m_nextPcmFrame * m_pcm.blockAlign());
    if (reader.read(offset, frames * m_pcm.blockAlign(), m_packet) != SourceReader::Status::Ok) {
        error = makeError(ErrorCode::ReadFailed, m_source->lastError().message);
        return Status::Error;
    }
    chunk.samples.resize(static_cast<std::size_t>(frames * m_pcm.channels));
    convertPcmToFloat(m_packet.data(), frames, m_pcm, chunk.samples.data());
    chunk.firstSample = m_nextPcmFrame;
    m_nextPcmFrame += frames;
    return Status::Ok;
}

Backend::Status IndexedBackend::seek(qint64 rawSample, qint64 &resumeAt, Error &error)
{
    resumeAt = -1;
    rawSample = std::max<qint64>(0, rawSample);
    if (m_kind == Kind::Pcm) {
        const qint64 total = m_pcm.frames();
        if (rawSample >= total) {
            m_nextPcmFrame = total;
            resumeAt = total;
            return Status::Ok;
        }
        const SourceSnapshot snapshot = m_source->snapshot();
        if (rawSample >= pcmAvailableFrames(snapshot) && !snapshot.isComplete()) {
            return outOfData(error);
        }
        m_nextPcmFrame = rawSample;
        resumeAt = rawSample;
        return Status::Ok;
    }
    // Index as far as the target, or as far as the bytes go.
    while (!m_index->finished && rawSample >= m_index->totalSamples) {
        const Extend extend = extendIndex(error);
        if (extend == Extend::Failed) {
            return Status::Error;
        }
        if (extend != Extend::Progress) {
            break;
        }
    }
    if (rawSample >= m_index->totalSamples) {
        if (!m_index->finished) {
            return outOfData(error);
        }
        m_nextFrame = m_index->frames.size();
        resumeAt = m_index->totalSamples;
        return Status::Ok;
    }
    std::size_t frame = m_index->frameFor(rawSample);
    if (frame >= m_index->frames.size()) {
        // In a numbering gap: resume at the next frame after it.
        const auto next =
            std::ranges::upper_bound(m_index->frames, rawSample, std::less<>{}, &IndexedFrame::firstSample);
        frame = static_cast<std::size_t>(std::distance(m_index->frames.begin(), next));
        if (frame >= m_index->frames.size()) {
            m_nextFrame = m_index->frames.size();
            resumeAt = m_index->totalSamples;
            return Status::Ok;
        }
    }
    const auto preroll = static_cast<std::size_t>(prerollFrames(m_kind));
    const std::size_t start = frame > preroll ? frame - preroll : 0;
    m_codec.flush();
    m_nextFrame = start;
    resumeAt = m_index->frames[start].firstSample;
    return Status::Ok;
}

qint64 IndexedBackend::decodableRawSamples()
{
    if (m_kind == Kind::Pcm) {
        return pcmAvailableFrames(m_source->snapshot());
    }
    Error ignored;
    while (extendIndex(ignored) == Extend::Progress) {
    }
    return m_index->totalSamples;
}

qint64 IndexedBackend::rawLength() const
{
    switch (m_kind) {
    case Kind::Pcm:
        return m_pcm.frames();
    case Kind::Flac:
        if (m_flac.totalSamples > 0) {
            return m_flac.totalSamples;
        }
        return m_index->finished ? m_index->totalSamples : -1;
    case Kind::Mpeg:
        break;
    }
    if (m_index->finished) {
        return m_index->totalSamples;
    }
    if (m_lame && m_lame->frames > 0) {
        return m_lame->frames * m_firstHeader.samplesPerFrame;
    }
    // Estimate from the stream size and the average frame size so far (exact for CBR).
    const auto frames = static_cast<qint64>(m_index->frames.size());
    const double averageBytes = frames > 0 ? static_cast<double>(m_index->totalBytes) / static_cast<double>(frames)
                                           : static_cast<double>(m_firstHeader.frameBytes);
    const qint64 streamBytes = m_source->totalSize() - m_firstAudioOffset;
    if (averageBytes <= 0 || streamBytes <= 0) {
        return -1;
    }
    return static_cast<qint64>(static_cast<double>(streamBytes) / averageBytes) * m_firstHeader.samplesPerFrame;
}

std::vector<DiscardWindow> IndexedBackend::discardWindows() const
{
    std::vector<DiscardWindow> windows;
    if (m_kind != Kind::Mpeg || !m_lame || m_lame->encoderDelay < 0) {
        return windows;
    }
    // FFmpeg's rule (mp3dec.c): delay and padding are trusted from LAME, Lavf and Lavc tags; the decoder's own delay
    // of 528 + 1 samples is added to both ends.
    const QString &encoder = m_lame->encoder;
    if (!encoder.startsWith(u"LAME"_s) && !encoder.startsWith(u"Lavf"_s) && !encoder.startsWith(u"Lavc"_s)) {
        return windows;
    }
    constexpr qint64 kDecoderDelay = 528 + 1;
    windows.push_back({0, m_lame->encoderDelay + kDecoderDelay});
    if (m_lame->frames > 0) {
        const qint64 spf = m_firstHeader.samplesPerFrame;
        const qint64 firstDiscard = (m_lame->frames * spf) - m_lame->encoderPadding + kDecoderDelay;
        const qint64 lastDiscard = m_lame->frames * spf;
        if (firstDiscard < lastDiscard) {
            windows.push_back({firstDiscard, lastDiscard});
        }
    }
    return windows;
}

DecoderStats IndexedBackend::stats() const
{
    DecoderStats stats;
    stats.decodeErrors = m_decodeErrors;
    if (m_kind == Kind::Pcm) {
        stats.truncated = m_pcm.truncated;
    } else if (m_index) {
        stats.skippedBytes = m_index->skippedBytes;
        stats.truncated = m_index->truncated;
    }
    return stats;
}

void IndexedBackend::finishInfo()
{
    m_info.progressive = true;
    m_info.exactSeek = true;
    m_info.audioOffset = m_kind == Kind::Pcm ? m_pcm.dataOffset : m_firstAudioOffset;
    switch (m_kind) {
    case Kind::Pcm:
        m_info.audioBytes = m_pcm.dataBytes;
        m_info.exactLength = true;
        m_info.bitrateKbps = static_cast<int>(
            (static_cast<qint64>(m_pcm.sampleRate) * m_pcm.channels * m_pcm.containerBytes * 8) / 1000);
        break;
    case Kind::Flac:
        m_info.exactLength = m_flac.totalSamples > 0 || m_index->finished;
        m_info.audioBytes = m_index->finished ? m_index->totalBytes : -1;
        if (m_index->finished && m_index->totalSamples > 0) {
            m_info.bitrateKbps = static_cast<int>(static_cast<double>(m_index->totalBytes) * 8.0 * m_flac.sampleRate /
                                                  static_cast<double>(m_index->totalSamples) / 1000.0);
        }
        break;
    case Kind::Mpeg: {
        m_info.exactLength = m_index->finished || (m_lame && m_lame->frames > 0);
        m_info.audioBytes = m_index->finished ? m_index->totalBytes : -1;
        MpegAudioStats mpeg;
        mpeg.version = m_firstHeader.version;
        mpeg.layer = m_firstHeader.layer;
        mpeg.channelMode = m_firstHeader.channelMode;
        mpeg.frameCount = static_cast<qint64>(m_index->frames.size());
        mpeg.minBitrateKbps = m_index->minBitrate;
        mpeg.maxBitrateKbps = m_index->maxBitrate;
        mpeg.skippedBytes = m_index->skippedBytes;
        m_info.mpeg = mpeg;
        if (m_index->totalSamples > 0) {
            m_info.bitrateKbps =
                static_cast<int>(std::lround(static_cast<double>(m_index->totalBytes) * 8.0 * m_firstHeader.sampleRate /
                                             static_cast<double>(m_index->totalSamples) / 1000.0));
        } else {
            m_info.bitrateKbps = m_firstHeader.bitrateKbps;
        }
        break;
    }
    }
}

Result<std::unique_ptr<Backend>> IndexedBackend::clone() const
{
    auto copy = std::make_unique<IndexedBackend>(m_kind, m_source, m_probe, m_options);
    if (m_kind != Kind::Pcm && m_index && m_index->finished) {
        copy->m_index = m_index;
        copy->m_indexShared = true;
    }
    Error error;
    const Status status = copy->open(error);
    if (status != Status::Ok) {
        if (!error.isError()) {
            error = makeError(ErrorCode::InvalidArgument, u"the source is not complete"_s);
        }
        return error;
    }
    return std::unique_ptr<Backend>(std::move(copy));
}

} // namespace selecta::audio::detail
