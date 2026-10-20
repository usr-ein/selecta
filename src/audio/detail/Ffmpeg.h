// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"

#include <QString>
#include <QtGlobal>

#include <memory>
#include <span>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

Q_DECLARE_LOGGING_CATEGORY(lcAudioFfmpeg)

// FFmpeg glue shared by the decoder backends and convert/'s encoders.
namespace selecta::audio::detail {

// FFmpeg's error constants are macros with C casts; these are the same values as typed constants.
int ffErrorEof();
int ffErrorEagain();
int ffErrorExit();
int ffErrorEio();
int ffErrorEnomem();
int ffErrorInvalidData();

QString ffmpegErrorString(int code);

// Tests and updates of FFmpeg's int flag fields without signed bitwise arithmetic.
constexpr bool hasFlag(int value, int flag)
{
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}
constexpr int withFlag(int value, int flag)
{
    return static_cast<int>(static_cast<unsigned>(value) | static_cast<unsigned>(flag));
}
constexpr int withoutFlag(int value, int flag)
{
    return static_cast<int>(static_cast<unsigned>(value) & ~static_cast<unsigned>(flag));
}

// Routes av_log to the "selecta.audio.ffmpeg" logging category (debug level) once per process. Called by every entry
// point that uses FFmpeg; cheap after the first call.
void installFfmpegLogging();

struct FormatContextDeleter
{
    void operator()(AVFormatContext *context) const;
};
struct CodecContextDeleter
{
    void operator()(AVCodecContext *context) const;
};
struct FrameDeleter
{
    void operator()(AVFrame *frame) const;
};
struct PacketDeleter
{
    void operator()(AVPacket *packet) const;
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// Appends `frame`'s samples as interleaved float, `channels` per frame. Any FFmpeg audio sample format, planar or
// packed. A frame with more channels than `channels` keeps the first ones; with fewer, the last is repeated. Returns
// the number of frames appended (0 for an unsupported format).
qint64 appendFrameAsFloat(const AVFrame *frame, int channels, std::vector<float> &out);

// An FFmpeg audio decoder fed one packet at a time.
//
// Threading: one thread at a time, like the Decoder that owns it.
class CodecDecoder
{
public:
    // Opens a decoder for `parameters` (from a demuxed stream). With `skipManual`, FFmpeg never trims priming or
    // padding (AV_CODEC_FLAG2_SKIP_MANUAL): the caller does it.
    Result<void> open(const AVCodecParameters *parameters, AVRational packetTimeBase, bool skipManual);
    // Opens a decoder from Selecta's own demuxers: codec, rate, channels and optional extradata (FLAC STREAMINFO).
    Result<void> open(AVCodecID codec, int sampleRate, int channels, std::span<const quint8> extradata,
                      bool skipManual);

    // Sends one packet and appends every decoded frame as interleaved float (channels() per frame). Returns the frame
    // count appended; a packet the codec rejects returns DecoderError (the caller decides whether to go on). `pts` and
    // the decoded frames' pts in packetTimeBase are reported through firstPts (AV_NOPTS_VALUE when unknown).
    Result<qint64> decode(const quint8 *data, int size, qint64 pts, std::vector<float> &out, qint64 *firstPts);
    // Sends a demuxed packet (FFmpeg backend).
    Result<qint64> decode(const AVPacket *packet, std::vector<float> &out, qint64 *firstPts);
    // Drains frames buffered inside the codec at the end of the stream.
    Result<qint64> drain(std::vector<float> &out, qint64 *firstPts);
    void flush();

    bool isOpen() const { return m_context != nullptr; }
    int channels() const { return m_channels; }
    int sampleRate() const { return m_sampleRate; }
    QString codecName() const;
    const AVCodecContext *context() const { return m_context.get(); }

private:
    Result<void> finishOpen(bool skipManual);
    Result<qint64> receiveAll(std::vector<float> &out, qint64 *firstPts);

    CodecContextPtr m_context;
    FramePtr m_frame;
    PacketPtr m_packet;
    int m_channels = 0;
    int m_sampleRate = 0;
};

} // namespace selecta::audio::detail
