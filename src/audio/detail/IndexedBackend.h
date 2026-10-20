// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/detail/Backend.h"
#include "audio/detail/ContainerProbe.h"
#include "audio/detail/Ffmpeg.h"
#include "audio/detail/FlacFormat.h"
#include "audio/detail/MpegAudio.h"
#include "audio/detail/PcmFormat.h"

#include <optional>

namespace selecta::audio::detail {

// One frame of an MPEG or FLAC stream, as indexed by Selecta.
struct IndexedFrame
{
    qint64 offset = 0;
    qint64 firstSample = 0;
    quint32 size = 0;
    quint32 samples = 0;
};

// The frame index of one stream. Grows while the source grows; immutable (and shared between clones) once the
// whole stream is indexed.
struct FrameIndex
{
    std::vector<IndexedFrame> frames;
    qint64 totalSamples = 0;
    qint64 totalBytes = 0;   // sum of frame sizes
    bool finished = false;   // every frame of the stream is indexed
    bool truncated = false;  // the stream ends inside a frame
    qint64 skippedBytes = 0; // junk skipped while resynchronising
    int minBitrate = 0;      // MPEG
    int maxBitrate = 0;

    // Index of the frame containing raw sample `sample` (frames are contiguous), or frames.size() past the end.
    std::size_t frameFor(qint64 sample) const;
};

// Selecta's own demuxers for MPEG audio, native FLAC, WAV and AIFF (see DESIGN.md): frames are indexed from the
// bytes available, and each complete frame goes to FFmpeg's codec as one packet (PCM is converted directly). Reads
// never pass the source's available count.
class IndexedBackend final : public Backend
{
public:
    enum class Kind { Mpeg, Flac, Pcm };

    // `headerOnly`: open() reads the format header (and the Xing/LAME frame) only, never indexes the whole file, and
    // the backend is for stream facts, not decoding (probeStreamInfo()).
    IndexedBackend(Kind kind, std::shared_ptr<ByteSource> source, const ProbeResult &probe,
                   const DecoderOptions &options, bool headerOnly = false);
    ~IndexedBackend() override;
    IndexedBackend(const IndexedBackend &) = delete;
    IndexedBackend &operator=(const IndexedBackend &) = delete;
    IndexedBackend(IndexedBackend &&) = delete;
    IndexedBackend &operator=(IndexedBackend &&) = delete;

    Status open(Error &error) override;
    const StreamInfo &info() const override { return m_info; }
    qint64 rawLength() const override;
    std::vector<DiscardWindow> discardWindows() const override;
    Status decode(DecodedChunk &chunk, Error &error) override;
    Status seek(qint64 rawSample, qint64 &resumeAt, Error &error) override;
    qint64 decodableRawSamples() override;
    DecoderStats stats() const override;
    Result<std::unique_ptr<Backend>> clone() const override;

    // Number of frames a seek decodes before the target (bit reservoir and filterbank warm-up), per kind.
    static int prerollFrames(Kind kind);

private:
    // A window of the source kept in memory for header and index scans.
    class BlockCache
    {
    public:
        // A pointer to `count` bytes at `position`, or nullptr when they aren't available (or on a read error, which
        // sets `failed`).
        const quint8 *peek(SourceReader &reader, qint64 position, qint64 count);
        void clear() { m_start = m_end = 0; }
        bool failed = false;
        qint64 blockBytes = 0; // the smallest read; set by the backend

    private:
        std::vector<quint8> m_data;
        qint64 m_start = 0;
        qint64 m_end = 0;
    };

    enum class Extend { Progress, Blocked, Done, Failed };
    enum class Skip { Continue, Finished, Blocked };
    enum class Candidate { Accept, Reject, Blocked };
    struct FlacSearch
    {
        enum class Result { Found, Blocked, NotFound };
        Result result = Result::NotFound;
        qint64 at = -1;
        std::optional<FlacFrameHeader> header;
    };

    Status unavailable(const SourceReader &reader, Error &error, const QString &cutShort) const;
    Status openMpeg(SourceReader &reader, Error &error);
    Status checkMpegRun(SourceReader &reader, Error &error);
    Status openFlac(SourceReader &reader, Error &error);
    Status readFlacMetadata(SourceReader &reader, qint64 &audioStart, Error &error);
    Status findFirstFlacFrame(SourceReader &reader, qint64 from, Error &error);
    Status openPcm(SourceReader &reader, Error &error);
    Extend extendIndex(Error &error);
    Extend extendMpeg(SourceReader &reader, Error &error);
    void appendMpegFrame(const MpegHeader &header);
    Skip skipNonFrame(SourceReader &reader, ByteSpan head, qint64 end);
    Extend extendFlac(SourceReader &reader, Error &error);
    void appendFlacFrame(const FlacFrameHeader &current, qint64 frameEnd);
    FlacSearch searchNextFlacFrame(SourceReader &reader, const FlacFrameHeader &current);
    Candidate checkFlacCandidate(SourceReader &reader, qint64 at, const FlacFrameHeader &current, qint64 limit,
                                 std::optional<FlacFrameHeader> &next);
    Status decodeFrames(DecodedChunk &chunk, Error &error);
    Status decodePcm(DecodedChunk &chunk, Error &error);
    Status outOfData(Error &error) const;
    void finishInfo();
    qint64 pcmAvailableFrames(SourceSnapshot snapshot) const;

    Kind m_kind;
    std::shared_ptr<ByteSource> m_source;
    ProbeResult m_probe;
    DecoderOptions m_options;
    StreamInfo m_info;
    CodecDecoder m_codec;
    std::shared_ptr<FrameIndex> m_index;
    bool m_indexShared = false; // m_index is shared with clones: never modify it
    bool m_headerOnly = false;
    BlockCache m_cache;
    std::vector<quint8> m_packet;
    std::size_t m_nextFrame = 0;
    qint64 m_decodeErrors = 0;
    qint64 m_scanPos = 0;

    // MPEG
    MpegHeader m_firstHeader;
    std::optional<LameTag> m_lame;
    qint64 m_firstAudioOffset = 0;

    // FLAC
    FlacStreamInfo m_flac;
    std::optional<FlacFrameHeader> m_pendingHeader; // header of the frame starting at m_scanPos
    qint64 m_searchFrom = 0;
    qint64 m_maxFrameBytes = 0;
    int m_fixedBlockSize = 0;

    // PCM
    PcmLayout m_pcm;
    qint64 m_nextPcmFrame = 0;
};

} // namespace selecta::audio::detail
