// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/detail/AvioAdapter.h"
#include "audio/detail/Backend.h"
#include "audio/detail/ContainerProbe.h"
#include "audio/detail/Ffmpeg.h"

namespace selecta::audio::detail {

// libavformat demuxing through an AvioAdapter: MP4/M4A (also while downloading, when `moov` comes first) and every
// other format FFmpeg reads (complete files only). Positions come from packet timestamps relative to the first
// decoded frame; seeks use av_seek_frame and the Decoder discards up to the exact target.
//
// While the source grows (MP4 fast start only), every packet read is gated on FFmpeg's own sample index: the next
// sample's bytes must have arrived. A read that still reaches the boundary (starvation) is discarded and the backend
// repositions to where it was once more bytes are there.
class FfmpegBackend final : public Backend
{
public:
    // `headerOnly`: open() reads what the demuxer needs to open the file and never decodes (probeStreamInfo()).
    FfmpegBackend(std::shared_ptr<ByteSource> source, const ProbeResult &probe, const DecoderOptions &options,
                  bool headerOnly = false);
    ~FfmpegBackend() override;
    FfmpegBackend(const FfmpegBackend &) = delete;
    FfmpegBackend &operator=(const FfmpegBackend &) = delete;
    FfmpegBackend(FfmpegBackend &&) = delete;
    FfmpegBackend &operator=(FfmpegBackend &&) = delete;

    Status open(Error &error) override;
    const StreamInfo &info() const override { return m_info; }
    qint64 rawLength() const override;
    std::vector<DiscardWindow> discardWindows() const override { return {}; }
    Status decode(DecodedChunk &chunk, Error &error) override;
    Status seek(qint64 rawSample, qint64 &resumeAt, Error &error) override;
    qint64 decodableRawSamples() override;
    DecoderStats stats() const override;
    Result<std::unique_ptr<Backend>> clone() const override;

private:
    Status readAndDecode(DecodedChunk &chunk, Error &error);
    Status endOfPackets(int ret, DecodedChunk &chunk, Error &error);
    void trackPacketEntry();
    bool dropBeforeTarget(DecodedChunk &chunk, qint64 frames);
    Status resumeAfterStarvation(Error &error);
    Status reposition(qint64 rawSample, Error &error);
    bool gateNextPacket();
    Status outOfData(Error &error) const;
    qint64 rawFromPts(qint64 pts) const;
    qint64 ptsFromRaw(qint64 raw) const;
    void fillInfo();

    std::shared_ptr<ByteSource> m_source;
    ProbeResult m_probe;
    DecoderOptions m_options;
    StreamInfo m_info;
    std::unique_ptr<AvioAdapter> m_avio;
    FormatContextPtr m_format;
    CodecDecoder m_codec;
    PacketPtr m_packet;
    int m_stream = -1;
    AVRational m_timeBase{1, 1};
    qint64 m_originPts = AV_NOPTS_VALUE;
    bool m_growing = false;
    bool m_headerOnly = false;
    int m_nextEntry = 0;      // index entry of the next packet (gating while growing)
    qint64 m_nextRaw = 0;     // running position for packets without timestamps
    qint64 m_dropBefore = -1; // after an internal reposition: discard decoded samples before this
    bool m_needReposition = false;
    qint64 m_repositionRaw = 0;
    qint64 m_starvedAt = 0;
    bool m_drained = false;
    bool m_truncated = false;
    qint64 m_decodeErrors = 0;
    DecodedChunk m_primed; // the first chunk, decoded during open() to find the time origin
    bool m_hasPrimed = false;
    bool m_findingOrigin = false;
};

} // namespace selecta::audio::detail
