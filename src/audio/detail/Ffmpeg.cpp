// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/Ffmpeg.h"

#include <QByteArray>

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcAudioFfmpeg, "selecta.audio.ffmpeg")

namespace selecta::audio::detail {

// The AVERROR macros expand to C casts in FFmpeg's headers; keeping them here leaves the rest of the code cast-free.
int ffErrorEof()
{
    return AVERROR_EOF; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}
int ffErrorEagain()
{
    return AVERROR(EAGAIN);
}
int ffErrorExit()
{
    return AVERROR_EXIT; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}
int ffErrorEio()
{
    return AVERROR(EIO);
}
int ffErrorEnomem()
{
    return AVERROR(ENOMEM);
}
int ffErrorInvalidData()
{
    return AVERROR_INVALIDDATA; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

QString ffmpegErrorString(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return u"FFmpeg error %1"_s.arg(code);
    }
    return QString::fromUtf8(buffer.data());
}

namespace {

void logCallback(void *object, int level, const char *format, va_list arguments)
{
    if (level > av_log_get_level() || !lcAudioFfmpeg().isDebugEnabled()) {
        return;
    }
    std::array<char, 1024> line{};
    int printPrefix = 1;
    av_log_format_line2(object, level, format, arguments, line.data(), static_cast<int>(line.size()), &printPrefix);
    QByteArray text(line.data());
    while (text.endsWith('\n')) {
        text.chop(1);
    }
    qCDebug(lcAudioFfmpeg).noquote() << QString::fromUtf8(text);
}

template <typename T>
T loadSample(const quint8 *plane, qint64 index)
{
    T value{};
    std::memcpy(&value, plane + (index * static_cast<qint64>(sizeof(T))), sizeof(T));
    return value;
}

template <typename T, typename Convert>
void convertChannels(const AVFrame *frame, bool planar, int srcChannels, int channels, float *dst, Convert convert)
{
    const qint64 n = frame->nb_samples;
    for (int c = 0; c < channels; ++c) {
        const int sc = std::min(c, srcChannels - 1);
        const quint8 *plane = planar ? frame->extended_data[sc] : frame->extended_data[0];
        const qint64 stride = planar ? 1 : srcChannels;
        const qint64 offset = planar ? 0 : sc;
        for (qint64 i = 0; i < n; ++i) {
            const float v = convert(loadSample<T>(plane, (i * stride) + offset));
            dst[(i * channels) + c] = std::isfinite(v) ? v : 0.0F;
        }
    }
}

} // namespace

void installFfmpegLogging()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(&logCallback);
    });
}

void FormatContextDeleter::operator()(AVFormatContext *context) const
{
    avformat_close_input(&context);
}
void CodecContextDeleter::operator()(AVCodecContext *context) const
{
    avcodec_free_context(&context);
}
void FrameDeleter::operator()(AVFrame *frame) const
{
    av_frame_free(&frame);
}
void PacketDeleter::operator()(AVPacket *packet) const
{
    av_packet_free(&packet);
}

qint64 appendFrameAsFloat(const AVFrame *frame, int channels, std::vector<float> &out)
{
    const int srcChannels = frame->ch_layout.nb_channels;
    if (frame->nb_samples <= 0 || srcChannels <= 0 || channels <= 0) {
        return 0;
    }
    const auto format = static_cast<AVSampleFormat>(frame->format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const std::size_t base = out.size();
    out.resize(base + (static_cast<std::size_t>(frame->nb_samples) * static_cast<std::size_t>(channels)));
    float *dst = out.data() + base;
    switch (av_get_packed_sample_fmt(format)) {
    case AV_SAMPLE_FMT_U8:
        convertChannels<quint8>(frame, planar, srcChannels, channels, dst,
                                [](quint8 v) { return static_cast<float>(static_cast<int>(v) - 128) / 128.0F; });
        break;
    case AV_SAMPLE_FMT_S16:
        convertChannels<qint16>(frame, planar, srcChannels, channels, dst,
                                [](qint16 v) { return static_cast<float>(v) / 32768.0F; });
        break;
    case AV_SAMPLE_FMT_S32:
        convertChannels<qint32>(frame, planar, srcChannels, channels, dst,
                                [](qint32 v) { return static_cast<float>(static_cast<double>(v) / 2147483648.0); });
        break;
    case AV_SAMPLE_FMT_S64:
        convertChannels<qint64>(frame, planar, srcChannels, channels, dst, [](qint64 v) {
            return static_cast<float>(static_cast<double>(v) / 9223372036854775808.0);
        });
        break;
    case AV_SAMPLE_FMT_FLT:
        convertChannels<float>(frame, planar, srcChannels, channels, dst, [](float v) { return v; });
        break;
    case AV_SAMPLE_FMT_DBL:
        convertChannels<double>(frame, planar, srcChannels, channels, dst,
                                [](double v) { return static_cast<float>(v); });
        break;
    default:
        out.resize(base);
        return 0;
    }
    return frame->nb_samples;
}

// ---------------------------------------------------------------------------------------------------------------
// CodecDecoder

Result<void> CodecDecoder::open(const AVCodecParameters *parameters, AVRational packetTimeBase, bool skipManual)
{
    installFfmpegLogging();
    m_context.reset();
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) {
        return makeError(ErrorCode::Unsupported,
                         u"no decoder for codec %1"_s.arg(QString::fromUtf8(avcodec_get_name(parameters->codec_id))));
    }
    m_context.reset(avcodec_alloc_context3(codec));
    if (!m_context) {
        return makeError(ErrorCode::Internal, u"out of memory allocating a decoder"_s);
    }
    if (const int ret = avcodec_parameters_to_context(m_context.get(), parameters); ret < 0) {
        return makeError(ErrorCode::DecoderError, ffmpegErrorString(ret));
    }
    m_context->pkt_timebase = packetTimeBase;
    return finishOpen(skipManual);
}

Result<void> CodecDecoder::open(AVCodecID codecId, int sampleRate, int channels, std::span<const quint8> extradata,
                                bool skipManual)
{
    installFfmpegLogging();
    m_context.reset();
    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) {
        return makeError(ErrorCode::Unsupported,
                         u"no decoder for codec %1"_s.arg(QString::fromUtf8(avcodec_get_name(codecId))));
    }
    m_context.reset(avcodec_alloc_context3(codec));
    if (!m_context) {
        return makeError(ErrorCode::Internal, u"out of memory allocating a decoder"_s);
    }
    m_context->sample_rate = sampleRate;
    av_channel_layout_default(&m_context->ch_layout, channels);
    m_context->pkt_timebase = AVRational{1, sampleRate};
    if (!extradata.empty()) {
        auto *buffer = static_cast<quint8 *>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (buffer == nullptr) {
            return makeError(ErrorCode::Internal, u"out of memory allocating codec extradata"_s);
        }
        std::memcpy(buffer, extradata.data(), extradata.size());
        m_context->extradata = buffer;
        m_context->extradata_size = static_cast<int>(extradata.size());
    }
    return finishOpen(skipManual);
}

Result<void> CodecDecoder::finishOpen(bool skipManual)
{
    if (skipManual) {
        m_context->flags2 = withFlag(m_context->flags2, AV_CODEC_FLAG2_SKIP_MANUAL);
    }
    m_context->thread_count = 1; // deterministic output, no frame-threading delay
    if (const int ret = avcodec_open2(m_context.get(), m_context->codec, nullptr); ret < 0) {
        m_context.reset();
        return makeError(ErrorCode::DecoderError, u"can't open the decoder: %1"_s.arg(ffmpegErrorString(ret)));
    }
    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    if (!m_frame || !m_packet) {
        m_context.reset();
        return makeError(ErrorCode::Internal, u"out of memory allocating decoder buffers"_s);
    }
    m_channels = m_context->ch_layout.nb_channels;
    m_sampleRate = m_context->sample_rate;
    return {};
}

QString CodecDecoder::codecName() const
{
    return m_context && m_context->codec != nullptr ? QString::fromUtf8(m_context->codec->name) : QString();
}

Result<qint64> CodecDecoder::receiveAll(std::vector<float> &out, qint64 *firstPts)
{
    qint64 frames = 0;
    bool first = true;
    while (true) {
        const int ret = avcodec_receive_frame(m_context.get(), m_frame.get());
        if (ret == ffErrorEagain() || ret == ffErrorEof()) {
            break;
        }
        if (ret < 0) {
            return makeError(ErrorCode::DecoderError, ffmpegErrorString(ret));
        }
        if (m_channels <= 0) {
            m_channels = m_frame->ch_layout.nb_channels;
        }
        if (m_sampleRate <= 0) {
            m_sampleRate = m_frame->sample_rate;
        }
        if (first && firstPts != nullptr) {
            *firstPts =
                m_frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_frame->best_effort_timestamp : m_frame->pts;
        }
        first = false;
        frames += appendFrameAsFloat(m_frame.get(), m_channels, out);
        av_frame_unref(m_frame.get());
    }
    return frames;
}

Result<qint64> CodecDecoder::decode(const quint8 *data, int size, qint64 pts, std::vector<float> &out, qint64 *firstPts)
{
    if (!m_context) {
        return makeError(ErrorCode::Internal, u"decoder not open"_s);
    }
    av_packet_unref(m_packet.get());
    if (const int ret = av_new_packet(m_packet.get(), size); ret < 0) {
        return makeError(ErrorCode::Internal, ffmpegErrorString(ret));
    }
    if (size > 0) {
        std::memcpy(m_packet->data, data, static_cast<std::size_t>(size));
    }
    m_packet->pts = pts;
    m_packet->dts = pts;
    return decode(m_packet.get(), out, firstPts);
}

Result<qint64> CodecDecoder::decode(const AVPacket *packet, std::vector<float> &out, qint64 *firstPts)
{
    if (!m_context) {
        return makeError(ErrorCode::Internal, u"decoder not open"_s);
    }
    if (firstPts != nullptr) {
        *firstPts = AV_NOPTS_VALUE;
    }
    int ret = avcodec_send_packet(m_context.get(), packet);
    qint64 frames = 0;
    if (ret == ffErrorEagain()) {
        // The codec holds output we haven't collected: collect it, then resend.
        Result<qint64> drained = receiveAll(out, firstPts);
        if (!drained) {
            return drained;
        }
        frames += drained.value();
        ret = avcodec_send_packet(m_context.get(), packet);
    }
    if (ret < 0) {
        return makeError(ErrorCode::DecoderError, ffmpegErrorString(ret));
    }
    qint64 pts = AV_NOPTS_VALUE;
    Result<qint64> received = receiveAll(out, &pts);
    if (!received) {
        return received;
    }
    if (firstPts != nullptr && *firstPts == AV_NOPTS_VALUE) {
        *firstPts = pts;
    }
    return frames + received.value();
}

Result<qint64> CodecDecoder::drain(std::vector<float> &out, qint64 *firstPts)
{
    if (!m_context) {
        return qint64{0};
    }
    if (firstPts != nullptr) {
        *firstPts = AV_NOPTS_VALUE;
    }
    const int ret = avcodec_send_packet(m_context.get(), nullptr);
    if (ret < 0 && ret != ffErrorEof()) {
        return makeError(ErrorCode::DecoderError, ffmpegErrorString(ret));
    }
    return receiveAll(out, firstPts);
}

void CodecDecoder::flush()
{
    if (m_context) {
        avcodec_flush_buffers(m_context.get());
    }
}

} // namespace selecta::audio::detail
