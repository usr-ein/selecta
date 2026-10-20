// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC, ALAC and AIFF through FFmpeg's encoders and muxers, writing into a Sink (conversion §2.3, §2.4).
#include "audio/detail/Ffmpeg.h"
#include "convert/MetadataModel.h"
#include "convert/detail/Encoder.h"
#include "convert/detail/Quantizer.h"

extern "C" {
#include <libavutil/mem.h>
}

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::convert::detail {
namespace {

namespace ff = audio::detail;

constexpr int kAvioBufferBytes = 64 * 1024;
constexpr int kDefaultFrameSize = 4096;
// AIFF chunk sizes are 32-bit.
constexpr qint64 kMaxAiffBytes = qint64{std::numeric_limits<quint32>::max()} - (qint64{64} * 1024);

struct OutputContextDeleter
{
    void operator()(AVFormatContext *context) const { avformat_free_context(context); }
};
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

struct AvioDeleter
{
    void operator()(AVIOContext *context) const
    {
        if (context != nullptr) {
            av_freep(static_cast<void *>(&context->buffer));
        }
        avio_context_free(&context);
    }
};
using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;

int writePacket(void *opaque, const uint8_t *data, int size)
{
    auto *sink = static_cast<Sink *>(opaque);
    if (const Result<void> written = sink->write(data, size); !written.ok()) {
        return written.error().code == ErrorCode::DiskFull ? AVERROR(ENOSPC) : ff::ffErrorEio();
    }
    return size;
}

int64_t seekPacket(void *opaque, int64_t offset, int whence)
{
    auto *sink = static_cast<Sink *>(opaque);
    const int mode = ff::withoutFlag(whence, AVSEEK_FORCE);
    if (mode == AVSEEK_SIZE) {
        return sink->size();
    }
    qint64 target = -1;
    if (mode == SEEK_SET) {
        target = offset;
    } else if (mode == SEEK_CUR) {
        target = sink->position() + offset;
    } else if (mode == SEEK_END) {
        target = sink->size() + offset;
    }
    if (target < 0) {
        return AVERROR(EINVAL);
    }
    return sink->seek(target).ok() ? target : ff::ffErrorEio();
}

template <typename T>
void storeSample(quint8 *plane, qint64 index, T value)
{
    std::memcpy(plane + (index * static_cast<qint64>(sizeof(T))), &value, sizeof(T));
}

struct CodecChoice
{
    AVCodecID codec = AV_CODEC_ID_NONE;
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
    const char *muxer = nullptr;
    QString name;
};

CodecChoice codecFor(OutputFormat format, int bitDepth)
{
    const bool deep = bitDepth == 24;
    switch (format) {
    case OutputFormat::Flac:
        return {AV_CODEC_ID_FLAC, deep ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16, "flac", u"FLAC"_s};
    case OutputFormat::Alac:
        return {AV_CODEC_ID_ALAC, deep ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S16P, "ipod", u"ALAC"_s};
    case OutputFormat::Aiff:
        // Big-endian PCM: FFmpeg's muxer writes FORM/AIFF for it, FORM/AIFC (which CDJs refuse) for anything else.
        return {deep ? AV_CODEC_ID_PCM_S24BE : AV_CODEC_ID_PCM_S16BE, deep ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16,
                "aiff", u"AIFF"_s};
    case OutputFormat::Mp3:
    case OutputFormat::Wav:
    case OutputFormat::KeepAudio:
        break;
    }
    return {};
}

class FfmpegEncoder final : public Encoder
{
public:
    FfmpegEncoder(Sink &sink, EncoderFormat pcm, CodecChoice choice)
        : m_sink(sink)
        , m_pcm(pcm)
        , m_choice(std::move(choice))
        , m_quantizer(pcm.bitDepth, pcm.dither)
    {}

    Result<void> open()
    {
        ff::installFfmpegLogging();
        const AVCodec *codec = avcodec_find_encoder(m_choice.codec);
        if (codec == nullptr) {
            return makeError(ErrorCode::EncoderError, u"this build has no %1 encoder"_s.arg(m_choice.name));
        }
        AVFormatContext *format = nullptr;
        if (avformat_alloc_output_context2(&format, nullptr, m_choice.muxer, nullptr) < 0 || format == nullptr) {
            return makeError(ErrorCode::EncoderError, u"this build has no %1 muxer"_s.arg(m_choice.name));
        }
        m_format.reset(format);

        auto *buffer = static_cast<unsigned char *>(av_malloc(kAvioBufferBytes));
        if (buffer == nullptr) {
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        m_avio.reset(avio_alloc_context(buffer, kAvioBufferBytes, /*write_flag=*/1, &m_sink, nullptr, &writePacket,
                                        &seekPacket));
        if (!m_avio) {
            av_free(buffer);
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        m_format->pb = m_avio.get();
        m_format->flags = ff::withFlag(m_format->flags, AVFMT_FLAG_CUSTOM_IO);

        m_codec.reset(avcodec_alloc_context3(codec));
        if (!m_codec) {
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        m_codec->sample_rate = m_pcm.sampleRate;
        m_codec->sample_fmt = m_choice.sampleFormat;
        m_codec->bits_per_raw_sample = m_pcm.bitDepth;
        m_codec->time_base = AVRational{1, m_pcm.sampleRate};
        av_channel_layout_default(&m_codec->ch_layout, m_pcm.channels);
        if (ff::hasFlag(m_format->oformat->flags, AVFMT_GLOBALHEADER)) {
            m_codec->flags = ff::withFlag(m_codec->flags, AV_CODEC_FLAG_GLOBAL_HEADER);
        }
        if (const int opened = avcodec_open2(m_codec.get(), codec, nullptr); opened < 0) {
            return makeError(ErrorCode::EncoderError,
                             u"FFmpeg's %1 encoder refused %2 Hz, %3 channels, %4-bit: %5"_s.arg(m_choice.name)
                                 .arg(m_pcm.sampleRate)
                                 .arg(m_pcm.channels)
                                 .arg(m_pcm.bitDepth)
                                 .arg(ff::ffmpegErrorString(opened)));
        }
        m_stream = avformat_new_stream(m_format.get(), nullptr);
        if (m_stream == nullptr || avcodec_parameters_from_context(m_stream->codecpar, m_codec.get()) < 0) {
            return makeError(ErrorCode::EncoderError, u"FFmpeg couldn't set up the %1 stream"_s.arg(m_choice.name));
        }
        m_stream->time_base = m_codec->time_base;
        if (const int written = avformat_write_header(m_format.get(), nullptr); written < 0) {
            return failure(written, u"write the %1 header"_s.arg(m_choice.name));
        }
        m_frameSize = m_codec->frame_size > 0 ? m_codec->frame_size : kDefaultFrameSize;
        m_frame.reset(av_frame_alloc());
        m_packet.reset(av_packet_alloc());
        if (!m_frame || !m_packet) {
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        m_frame->format = m_choice.sampleFormat;
        m_frame->sample_rate = m_pcm.sampleRate;
        m_frame->nb_samples = m_frameSize;
        if (av_channel_layout_copy(&m_frame->ch_layout, &m_codec->ch_layout) < 0 ||
            av_frame_get_buffer(m_frame.get(), 0) < 0) {
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        return {};
    }

    Result<void> write(const float *samples, qint64 frames) override
    {
        const auto count = static_cast<std::size_t>(frames * m_pcm.channels);
        const std::size_t start = m_pending.size();
        m_pending.resize(start + count);
        m_quantizer.quantize({samples, count}, {m_pending.data() + start, count});
        return sendPending(/*all=*/false);
    }

    Result<void> finish() override
    {
        if (const Result<void> sent = sendPending(/*all=*/true); !sent.ok()) {
            return sent;
        }
        if (const int flushed = avcodec_send_frame(m_codec.get(), nullptr); flushed < 0) {
            return failure(flushed, u"finish the %1 stream"_s.arg(m_choice.name));
        }
        if (const Result<void> drained = receivePackets(); !drained.ok()) {
            return drained;
        }
        if (const int trailer = av_write_trailer(m_format.get()); trailer < 0) {
            return failure(trailer, u"complete the %1 file"_s.arg(m_choice.name));
        }
        avio_flush(m_avio.get());
        if (m_sink.error().isError()) {
            return m_sink.error();
        }
        if (m_avio->error < 0) {
            return failure(m_avio->error, u"write the %1 file"_s.arg(m_choice.name));
        }
        return checkSize();
    }

    qint64 clippedSamples() const override { return m_quantizer.clippedSamples(); }

private:
    // The sink's own error (a full disk) explains an FFmpeg failure better than FFmpeg's code for it.
    Result<void> failure(int code, const QString &what) const
    {
        if (m_sink.error().isError()) {
            return m_sink.error();
        }
        return makeError(ErrorCode::EncoderError, u"FFmpeg couldn't %1: %2"_s.arg(what, ff::ffmpegErrorString(code)));
    }

    Result<void> checkSize() const
    {
        if (m_choice.codec != AV_CODEC_ID_FLAC && m_choice.codec != AV_CODEC_ID_ALAC && m_sink.size() > kMaxAiffBytes) {
            return makeError(ErrorCode::TooLarge, u"AIFF files can't be larger than 4 GiB"_s);
        }
        return {};
    }

    // Sends whole frames of m_frameSize from m_pending (and the remainder too when `all`).
    Result<void> sendPending(bool all)
    {
        const auto channels = static_cast<std::size_t>(m_pcm.channels);
        std::size_t offset = 0;
        while (true) {
            const std::size_t available = (m_pending.size() - offset) / channels;
            if (available == 0 || (!all && std::cmp_less(available, m_frameSize))) {
                break;
            }
            const int frames = static_cast<int>(std::min(available, static_cast<std::size_t>(m_frameSize)));
            if (const Result<void> sent = sendFrame(m_pending.data() + offset, frames); !sent.ok()) {
                return sent;
            }
            offset += static_cast<std::size_t>(frames) * channels;
        }
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(offset));
        return checkSize();
    }

    Result<void> sendFrame(const qint32 *samples, int frames)
    {
        if (av_frame_make_writable(m_frame.get()) < 0) {
            return makeError(ErrorCode::EncoderError, u"out of memory"_s);
        }
        m_frame->nb_samples = frames;
        m_frame->pts = m_pts;
        m_pts += frames;
        fillFrame(samples, frames);
        if (const int sent = avcodec_send_frame(m_codec.get(), m_frame.get()); sent < 0) {
            return failure(sent, u"encode %1"_s.arg(m_choice.name));
        }
        return receivePackets();
    }

    void fillFrame(const qint32 *samples, int frames)
    {
        const int channels = m_pcm.channels;
        // 24-bit audio goes into 32-bit samples left-justified (the encoders shift by 32 − bits_per_raw_sample).
        const qint32 scale = m_pcm.bitDepth == 24 ? 256 : 1;
        const qint64 total = qint64{frames} * channels;
        switch (m_choice.sampleFormat) {
        case AV_SAMPLE_FMT_S16:
            for (qint64 i = 0; i < total; ++i) {
                storeSample(m_frame->data[0], i, static_cast<qint16>(samples[i]));
            }
            break;
        case AV_SAMPLE_FMT_S32:
            for (qint64 i = 0; i < total; ++i) {
                storeSample(m_frame->data[0], i, samples[i] * scale);
            }
            break;
        case AV_SAMPLE_FMT_S16P:
        case AV_SAMPLE_FMT_S32P:
            for (int c = 0; c < channels; ++c) {
                quint8 *plane = m_frame->extended_data[c];
                for (qint64 i = 0; i < frames; ++i) {
                    const qint32 value = samples[(i * channels) + c];
                    if (m_choice.sampleFormat == AV_SAMPLE_FMT_S16P) {
                        storeSample(plane, i, static_cast<qint16>(value));
                    } else {
                        storeSample(plane, i, value * scale);
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    Result<void> receivePackets()
    {
        while (true) {
            const int received = avcodec_receive_packet(m_codec.get(), m_packet.get());
            if (received == ff::ffErrorEagain() || received == ff::ffErrorEof()) {
                return {};
            }
            if (received < 0) {
                return failure(received, u"encode %1"_s.arg(m_choice.name));
            }
            m_packet->stream_index = m_stream->index;
            av_packet_rescale_ts(m_packet.get(), m_codec->time_base, m_stream->time_base);
            const int written = av_write_frame(m_format.get(), m_packet.get());
            av_packet_unref(m_packet.get());
            if (written < 0) {
                return failure(written, u"write %1"_s.arg(m_choice.name));
            }
        }
    }

    Sink &m_sink;
    EncoderFormat m_pcm;
    CodecChoice m_choice;
    Quantizer m_quantizer;
    // Declared in this order so the muxer goes before the I/O context it writes to, and the codec before both.
    AvioPtr m_avio;
    OutputContextPtr m_format;
    ff::CodecContextPtr m_codec;
    AVStream *m_stream = nullptr; // owned by m_format
    ff::FramePtr m_frame;
    ff::PacketPtr m_packet;
    std::vector<qint32> m_pending; // quantised, interleaved, not yet a whole frame
    int m_frameSize = kDefaultFrameSize;
    qint64 m_pts = 0;
};

} // namespace

Result<std::unique_ptr<Encoder>> createFfmpegEncoder(OutputFormat format, EncoderFormat pcm, Sink &sink)
{
    CodecChoice choice = codecFor(format, pcm.bitDepth);
    if (choice.codec == AV_CODEC_ID_NONE) {
        return makeError(ErrorCode::InvalidArgument, u"FFmpeg encodes only FLAC, ALAC and AIFF here"_s);
    }
    if (pcm.bitDepth != 16 && pcm.bitDepth != 24) {
        return makeError(ErrorCode::InvalidArgument,
                         u"%1 output is 16- or 24-bit, not %2-bit"_s.arg(choice.name).arg(pcm.bitDepth));
    }
    if (pcm.channels < 1 || pcm.channels > 8 || pcm.sampleRate < 1) {
        return makeError(ErrorCode::InvalidArgument,
                         u"%1 Hz with %2 channels can't be encoded"_s.arg(pcm.sampleRate).arg(pcm.channels));
    }
    auto encoder = std::make_unique<FfmpegEncoder>(sink, pcm, std::move(choice));
    if (const Result<void> opened = encoder->open(); !opened.ok()) {
        return opened.error();
    }
    return std::unique_ptr<Encoder>(std::move(encoder));
}

} // namespace selecta::convert::detail
