// SPDX-License-Identifier: GPL-3.0-or-later
#include "SyntheticAudio.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <random>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/tx.h>
}

#include <lame/lame.h>

using namespace Qt::StringLiterals;

namespace selecta::testing {
namespace {

Pcm make(int sampleRate, int channels, double seconds)
{
    Pcm pcm;
    pcm.sampleRate = sampleRate;
    pcm.channels = channels;
    pcm.samples.assign(static_cast<std::size_t>(std::llround(seconds * sampleRate)) * channels, 0.0F);
    return pcm;
}

void appendBe16(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>((v >> 8U) & 0xFFU));
    out.append(static_cast<char>(v & 0xFFU));
}
void appendBe32(QByteArray &out, quint32 v)
{
    appendBe16(out, v >> 16U);
    appendBe16(out, v & 0xFFFFU);
}
void appendLe16(QByteArray &out, quint32 v)
{
    out.append(static_cast<char>(v & 0xFFU));
    out.append(static_cast<char>((v >> 8U) & 0xFFU));
}
void appendLe32(QByteArray &out, quint32 v)
{
    appendLe16(out, v & 0xFFFFU);
    appendLe16(out, v >> 16U);
}

qint32 toInt(float sample, int bits)
{
    const double scale = std::ldexp(1.0, bits - 1);
    const double v = std::round(static_cast<double>(std::clamp(sample, -1.0F, 1.0F)) * scale);
    return static_cast<qint32>(std::clamp(v, -scale, scale - 1.0));
}

void appendSample(QByteArray &out, float sample, int bits, bool floatingPoint, bool bigEndian)
{
    if (floatingPoint) {
        if (bits == 64) {
            quint64 raw = 0;
            const double d = sample;
            std::memcpy(&raw, &d, 8);
            for (int i = 0; i < 8; ++i) {
                const int shift = bigEndian ? (56 - (8 * i)) : (8 * i);
                out.append(static_cast<char>((raw >> static_cast<unsigned>(shift)) & 0xFFU));
            }
        } else {
            quint32 raw = 0;
            std::memcpy(&raw, &sample, 4);
            if (bigEndian) {
                appendBe32(out, raw);
            } else {
                appendLe32(out, raw);
            }
        }
        return;
    }
    if (bits == 8 && !bigEndian) { // WAV 8-bit is unsigned
        out.append(static_cast<char>(static_cast<quint8>(toInt(sample, 8) + 128)));
        return;
    }
    const auto v = static_cast<quint32>(toInt(sample, bits));
    const int bytes = (bits + 7) / 8;
    // Left-justify words that don't fill their bytes (AIFF convention; WAV writes 8/16/24/32 only).
    const quint32 word = v << static_cast<unsigned>((bytes * 8) - bits);
    for (int i = 0; i < bytes; ++i) {
        const int shift = bigEndian ? (8 * (bytes - 1 - i)) : (8 * i);
        out.append(static_cast<char>((word >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

QByteArray id3v2Tag(int totalBytes, unsigned seed)
{
    totalBytes = std::max(totalBytes, 32);
    const int body = totalBytes - 10;
    const int frameData = body - 10;
    QByteArray tag("ID3\x03\x00\x00", 6);
    const auto size = static_cast<quint32>(body);
    tag.append(static_cast<char>((size >> 21U) & 0x7FU));
    tag.append(static_cast<char>((size >> 14U) & 0x7FU));
    tag.append(static_cast<char>((size >> 7U) & 0x7FU));
    tag.append(static_cast<char>(size & 0x7FU));
    tag.append("PRIV", 4);
    appendBe32(tag, static_cast<quint32>(frameData));
    tag.append(2, '\0');
    QByteArray data = "selecta-test"_ba;
    data.append('\0');
    // Random bytes stand in for a picture: they contain plenty of false MPEG and FLAC sync patterns.
    data.append(randomBytes(frameData - static_cast<int>(data.size()), seed));
    tag.append(data.left(frameData));
    return tag;
}

void writeExtended80(QByteArray &out, double value)
{
    int exponent = 0;
    const double mantissa = std::frexp(value, &exponent); // value = mantissa · 2^exponent, mantissa in [0.5, 1)
    const auto bits = static_cast<quint64>(std::ldexp(mantissa, 64));
    const auto biased = static_cast<quint32>(exponent - 1 + 16383);
    appendBe16(out, biased & 0x7FFFU);
    appendBe32(out, static_cast<quint32>(bits >> 32U));
    appendBe32(out, static_cast<quint32>(bits & 0xFFFFFFFFU));
}

struct FfmpegFile
{
    const char *muxer = nullptr;
    const char *extension = nullptr;
    AVCodecID codec = AV_CODEC_ID_NONE;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;
    int bitsPerRawSample = 0;
    qint64 bitRate = 0;
    const char *movflags = nullptr;
};

void fill(const AVFrame *frame, const Pcm &pcm, qint64 first, int count, AVSampleFormat format, int bits)
{
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    for (int c = 0; c < pcm.channels; ++c) {
        for (int i = 0; i < count; ++i) {
            const float s = pcm.at(first + i, c);
            const qint64 index = planar ? i : (qint64{i} * pcm.channels) + c;
            quint8 *plane = frame->extended_data[planar ? c : 0];
            switch (av_get_packed_sample_fmt(format)) {
            case AV_SAMPLE_FMT_S16: {
                const auto v = static_cast<qint16>(toInt(s, 16));
                std::memcpy(plane + (index * 2), &v, 2);
                break;
            }
            case AV_SAMPLE_FMT_S32: {
                const auto v =
                    static_cast<qint32>(static_cast<quint32>(toInt(s, bits)) << static_cast<unsigned>(32 - bits));
                std::memcpy(plane + (index * 4), &v, 4);
                break;
            }
            case AV_SAMPLE_FMT_FLT:
                std::memcpy(plane + (index * 4), &s, 4);
                break;
            default:
                throw std::runtime_error("unsupported sample format in test encoder");
            }
        }
    }
}

void check(int ret, const char *what)
{
    if (ret < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
        av_strerror(ret, text.data(), text.size());
        throw std::runtime_error(std::string(what) + ": " + text.data());
    }
}

QByteArray encodeWithFfmpeg(const Pcm &pcm, const FfmpegFile &spec)
{
    const QTemporaryDir dir;
    if (!dir.isValid()) {
        throw std::runtime_error("no temporary directory");
    }
    const QByteArray path = QFile::encodeName(dir.filePath(u"out."_s + QString::fromLatin1(spec.extension)));
    AVFormatContext *output = nullptr;
    check(avformat_alloc_output_context2(&output, nullptr, spec.muxer, path.constData()), "output context");
    const AVCodec *codec = avcodec_find_encoder(spec.codec);
    if (codec == nullptr) {
        throw std::runtime_error("encoder missing");
    }
    AVStream *stream = avformat_new_stream(output, nullptr);
    AVCodecContext *encoder = avcodec_alloc_context3(codec);
    encoder->sample_rate = pcm.sampleRate;
    av_channel_layout_default(&encoder->ch_layout, pcm.channels);
    encoder->sample_fmt = spec.format;
    encoder->bits_per_raw_sample = spec.bitsPerRawSample;
    encoder->time_base = AVRational{1, pcm.sampleRate};
    encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (spec.bitRate > 0) {
        encoder->bit_rate = spec.bitRate;
    }
    if ((static_cast<unsigned>(output->oformat->flags) & static_cast<unsigned>(AVFMT_GLOBALHEADER)) != 0) {
        encoder->flags = static_cast<int>(static_cast<unsigned>(encoder->flags) |
                                          static_cast<unsigned>(AV_CODEC_FLAG_GLOBAL_HEADER));
    }
    check(avcodec_open2(encoder, codec, nullptr), "open encoder");
    check(avcodec_parameters_from_context(stream->codecpar, encoder), "parameters");
    stream->time_base = encoder->time_base;
    check(avio_open(&output->pb, path.constData(), AVIO_FLAG_WRITE), "open output");
    AVDictionary *options = nullptr;
    if (spec.movflags != nullptr) {
        av_dict_set(&options, "movflags", spec.movflags, 0);
    }
    check(avformat_write_header(output, &options), "write header");
    av_dict_free(&options);

    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 4096;
    const auto drain = [&] {
        while (true) {
            const int ret = avcodec_receive_packet(encoder, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
                break;
            }
            check(ret, "receive packet");
            av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(output, packet), "write packet");
        }
    };
    for (qint64 first = 0; first < pcm.frames(); first += frameSize) {
        const int count = static_cast<int>(std::min<qint64>(frameSize, pcm.frames() - first));
        frame->nb_samples = count;
        frame->format = spec.format;
        frame->sample_rate = pcm.sampleRate;
        av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
        check(av_frame_get_buffer(frame, 0), "frame buffer");
        fill(frame, pcm, first, count, spec.format, spec.bitsPerRawSample > 0 ? spec.bitsPerRawSample : 32);
        frame->pts = first;
        check(avcodec_send_frame(encoder, frame), "send frame");
        av_frame_unref(frame);
        drain();
    }
    check(avcodec_send_frame(encoder, nullptr), "flush encoder");
    drain();
    check(av_write_trailer(output), "trailer");
    avio_closep(&output->pb);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    avformat_free_context(output);

    QFile file(QFile::decodeName(path));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("can't read the encoded file back");
    }
    return file.readAll();
}

} // namespace

QByteArray randomBytes(int count, unsigned seed)
{
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(0, 255);
    QByteArray bytes(std::max(0, count), '\0');
    for (char &b : bytes) {
        b = static_cast<char>(distribution(generator));
    }
    return bytes;
}

Pcm sine(double frequency, double seconds, int sampleRate, int channels, double amplitude)
{
    Pcm pcm = make(sampleRate, channels, seconds);
    for (qint64 i = 0; i < pcm.frames(); ++i) {
        const auto v = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / sampleRate));
        for (int c = 0; c < channels; ++c) {
            pcm.samples[static_cast<std::size_t>((i * channels) + c)] = v;
        }
    }
    return pcm;
}

Pcm tones(std::initializer_list<double> frequencies, double seconds, int sampleRate, int channels, double amplitude)
{
    Pcm pcm = make(sampleRate, channels, seconds);
    for (qint64 i = 0; i < pcm.frames(); ++i) {
        double v = 0;
        for (const double f : frequencies) {
            v += amplitude * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / sampleRate);
        }
        for (int c = 0; c < channels; ++c) {
            pcm.samples[static_cast<std::size_t>((i * channels) + c)] = static_cast<float>(v);
        }
    }
    return pcm;
}

Pcm logSweep(double from, double to, double seconds, int sampleRate, int channels, double amplitude)
{
    Pcm pcm = make(sampleRate, channels, seconds);
    const double k = std::log(to / from);
    for (qint64 i = 0; i < pcm.frames(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double phase = 2.0 * std::numbers::pi * from * seconds / k * (std::exp(t / seconds * k) - 1.0);
        const auto v = static_cast<float>(amplitude * std::sin(phase));
        for (int c = 0; c < channels; ++c) {
            pcm.samples[static_cast<std::size_t>((i * channels) + c)] = v;
        }
    }
    return pcm;
}

Pcm whiteNoise(double seconds, unsigned seed, int sampleRate, int channels, double amplitude)
{
    Pcm pcm = make(sampleRate, channels, seconds);
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> distribution(-amplitude, amplitude);
    for (float &s : pcm.samples) {
        s = static_cast<float>(distribution(generator));
    }
    return pcm;
}

Pcm brickwallNoise(double cutoffHz, double seconds, unsigned seed, int sampleRate, int channels, double rms)
{
    constexpr int kPeriod = 131072; // 2^17
    AVTXContext *context = nullptr;
    av_tx_fn inverse = nullptr;
    const float scale = 1.0F;
    if (av_tx_init(&context, &inverse, AV_TX_FLOAT_RDFT, 1, kPeriod, &scale, AV_TX_UNALIGNED) < 0) {
        throw std::runtime_error("no FFT for the test signal");
    }
    std::vector<AVComplexFloat> spectrum((kPeriod / 2) + 1, AVComplexFloat{0.0F, 0.0F});
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> phase(0.0, 2.0 * std::numbers::pi);
    for (std::size_t k = 1; k < spectrum.size(); ++k) {
        const double phi = phase(random);
        if (static_cast<double>(k) * sampleRate / kPeriod < cutoffHz) {
            spectrum[k] = {static_cast<float>(std::cos(phi)), static_cast<float>(std::sin(phi))};
        }
    }
    std::vector<float> period(kPeriod);
    inverse(context, period.data(), spectrum.data(), sizeof(AVComplexFloat));
    av_tx_uninit(&context);
    double power = 0.0;
    for (const float x : period) {
        power += double{x} * x;
    }
    const auto gain = static_cast<float>(rms / std::sqrt(power / kPeriod));
    Pcm pcm = make(sampleRate, channels, seconds);
    for (qint64 f = 0; f < pcm.frames(); ++f) {
        const float x = period[static_cast<std::size_t>(f % kPeriod)] * gain;
        for (int c = 0; c < channels; ++c) {
            pcm.samples[static_cast<std::size_t>((f * channels) + c)] = x;
        }
    }
    return pcm;
}

Pcm silence(double seconds, int sampleRate, int channels)
{
    return make(sampleRate, channels, seconds);
}

Pcm stereoPair(double left, double right, double seconds, int sampleRate, double amplitude)
{
    Pcm pcm = make(sampleRate, 2, seconds);
    for (qint64 i = 0; i < pcm.frames(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        pcm.samples[static_cast<std::size_t>(2 * i)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * left * t));
        pcm.samples[static_cast<std::size_t>((2 * i) + 1)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * right * t));
    }
    return pcm;
}

Pcm quantised(const Pcm &pcm, int bits)
{
    Pcm out = pcm;
    const double scale = std::ldexp(1.0, bits - 1);
    for (float &s : out.samples) {
        s = static_cast<float>(static_cast<double>(toInt(s, bits)) / scale);
    }
    return out;
}

Pcm concatenated(const Pcm &a, const Pcm &b)
{
    Pcm out = a;
    out.samples.insert(out.samples.end(), b.samples.begin(), b.samples.end());
    return out;
}

QByteArray makeWav(const Pcm &pcm, const WavOptions &options)
{
    const int bytesPerSample = options.bits / 8;
    const int blockAlign = bytesPerSample * pcm.channels;
    QByteArray data;
    data.reserve(static_cast<qsizetype>(pcm.samples.size()) * bytesPerSample);
    for (const float s : pcm.samples) {
        appendSample(data, s, options.bits, options.floatingPoint, false);
    }
    QByteArray fmt;
    const quint32 tag = options.floatingPoint ? 3U : 1U;
    appendLe16(fmt, options.extensible ? 0xFFFEU : tag);
    appendLe16(fmt, static_cast<quint32>(pcm.channels));
    appendLe32(fmt, static_cast<quint32>(pcm.sampleRate));
    appendLe32(fmt, static_cast<quint32>(pcm.sampleRate * blockAlign));
    appendLe16(fmt, static_cast<quint32>(blockAlign));
    appendLe16(fmt, static_cast<quint32>(options.bits));
    if (options.extensible) {
        appendLe16(fmt, 22);
        appendLe16(fmt, static_cast<quint32>(options.bits));
        appendLe32(fmt, pcm.channels == 2 ? 3U : 4U);
        appendLe16(fmt, tag);
        fmt.append("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    }
    const auto chunk = [](const char *id, const QByteArray &body, quint32 sizeField) {
        QByteArray c(id, 4);
        appendLe32(c, sizeField);
        c.append(body);
        if (body.size() % 2 != 0) {
            c.append('\0');
        }
        return c;
    };
    QByteArray body("WAVE");
    if (options.listChunkBytes > 0) {
        QByteArray list("INFOISFT", 8);
        appendLe32(list, static_cast<quint32>(options.listChunkBytes));
        list.append(QByteArray(options.listChunkBytes, 'x'));
        body.append(chunk("LIST", list, static_cast<quint32>(list.size())));
    }
    const QByteArray dataChunk =
        chunk("data", data, options.unknownDataSize ? 0xFFFFFFFFU : static_cast<quint32>(data.size()));
    const QByteArray fmtChunk = chunk("fmt ", fmt, static_cast<quint32>(fmt.size()));
    if (options.dataBeforeFormat) {
        body.append(dataChunk);
        body.append(fmtChunk);
    } else {
        body.append(fmtChunk);
        body.append(dataChunk);
    }
    if (options.trailingChunkBytes > 0) {
        body.append(chunk("id3 ", randomBytes(options.trailingChunkBytes, 7),
                          static_cast<quint32>(options.trailingChunkBytes)));
    }
    QByteArray file("RIFF");
    appendLe32(file, static_cast<quint32>(body.size()));
    file.append(body);
    return file;
}

QByteArray makeAiff(const Pcm &pcm, const AiffOptions &options)
{
    const bool aifc = !options.compression.isEmpty();
    const bool littleEndian = options.compression == "sowt";
    const bool floating = options.compression == "fl32";
    QByteArray samples;
    for (const float s : pcm.samples) {
        appendSample(samples, s, floating ? 32 : options.bits, floating, !littleEndian);
    }
    QByteArray comm;
    appendBe16(comm, static_cast<quint32>(pcm.channels));
    appendBe32(comm, static_cast<quint32>(pcm.frames()));
    appendBe16(comm, static_cast<quint32>(floating ? 32 : options.bits));
    writeExtended80(comm, pcm.sampleRate);
    if (aifc) {
        comm.append(options.compression.left(4));
        comm.append('\x04');
        comm.append("test", 4);
        comm.append('\0'); // pad the Pascal string to even length
    }
    QByteArray ssnd;
    appendBe32(ssnd, 0);
    appendBe32(ssnd, 0);
    ssnd.append(samples);
    const auto chunk = [](const char *id, const QByteArray &body) {
        QByteArray c(id, 4);
        appendBe32(c, static_cast<quint32>(body.size()));
        c.append(body);
        if (body.size() % 2 != 0) {
            c.append('\0');
        }
        return c;
    };
    QByteArray body(aifc ? "AIFC" : "AIFF");
    if (aifc) {
        QByteArray fver;
        appendBe32(fver, 0xA2805140U);
        body.append(chunk("FVER", fver));
    }
    if (options.id3ChunkBytes > 0) {
        body.append(chunk("ID3 ", id3v2Tag(options.id3ChunkBytes, 3)));
    }
    if (options.commAfterSsnd) {
        body.append(chunk("SSND", ssnd));
        body.append(chunk("COMM", comm));
    } else {
        body.append(chunk("COMM", comm));
        body.append(chunk("SSND", ssnd));
    }
    QByteArray file("FORM");
    appendBe32(file, static_cast<quint32>(body.size()));
    file.append(body);
    return file;
}

QByteArray makeMp3(const Pcm &pcm, const Mp3Options &options)
{
    lame_t lame = lame_init();
    lame_set_num_channels(lame, pcm.channels);
    lame_set_in_samplerate(lame, pcm.sampleRate);
    lame_set_out_samplerate(lame, pcm.sampleRate);
    lame_set_mode(lame, pcm.channels == 1 ? MONO : JOINT_STEREO);
    if (options.vbr) {
        lame_set_VBR(lame, vbr_default);
        lame_set_VBR_q(lame, options.vbrQuality);
    } else {
        lame_set_VBR(lame, vbr_off);
        lame_set_brate(lame, options.bitrateKbps);
    }
    lame_set_quality(lame, 3);
    lame_set_bWriteVbrTag(lame, options.lameTag ? 1 : 0);
    if (lame_init_params(lame) < 0) {
        lame_close(lame);
        throw std::runtime_error("lame_init_params failed");
    }
    QByteArray mp3;
    constexpr int kBlock = 4096;
    std::vector<unsigned char> buffer((kBlock * 5 / 4) + 7200);
    for (qint64 first = 0; first < pcm.frames(); first += kBlock) {
        const int count = static_cast<int>(std::min<qint64>(kBlock, pcm.frames() - first));
        const float *data = pcm.samples.data() + (first * pcm.channels);
        int written = 0;
        if (pcm.channels == 2) {
            written = lame_encode_buffer_interleaved_ieee_float(lame, data, count, buffer.data(),
                                                                static_cast<int>(buffer.size()));
        } else {
            written = lame_encode_buffer_ieee_float(lame, data, nullptr, count, buffer.data(),
                                                    static_cast<int>(buffer.size()));
        }
        if (written < 0) {
            lame_close(lame);
            throw std::runtime_error("lame encode failed");
        }
        mp3.append(reinterpret_cast<const char *>(buffer.data()), written);
    }
    const int tail = lame_encode_flush(lame, buffer.data(), static_cast<int>(buffer.size()));
    if (tail > 0) {
        mp3.append(reinterpret_cast<const char *>(buffer.data()), tail);
    }
    if (options.lameTag) {
        std::array<unsigned char, 2880> tag{};
        const size_t size = lame_get_lametag_frame(lame, tag.data(), tag.size());
        if (size > 0 && std::cmp_less_equal(size, mp3.size())) {
            std::memcpy(mp3.data(), tag.data(), size);
        }
    }
    lame_close(lame);
    QByteArray file;
    if (options.id3v2Bytes > 0) {
        file.append(id3v2Tag(options.id3v2Bytes, 11));
    }
    if (options.junkBytes > 0) {
        file.append(QByteArray(options.junkBytes, '\0'));
    }
    file.append(mp3);
    if (options.id3v1) {
        QByteArray tag("TAG");
        tag.append(QByteArray(125, ' '));
        file.append(tag);
    }
    return file;
}

QByteArray makeFlac(const Pcm &pcm, FlacOptions options)
{
    FfmpegFile spec;
    spec.muxer = "flac";
    spec.extension = "flac";
    spec.codec = AV_CODEC_ID_FLAC;
    spec.format = options.bits > 16 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    spec.bitsPerRawSample = options.bits;
    QByteArray flac = encodeWithFfmpeg(pcm, spec);
    if (options.pictureBytes > 0 && flac.startsWith("fLaC") && flac.size() > 42) {
        // Insert a PICTURE block right after STREAMINFO (which FFmpeg never writes as the last block).
        QByteArray picture;
        appendBe32(picture, 3); // front cover
        appendBe32(picture, 10);
        picture.append("image/jpeg");
        appendBe32(picture, 0);
        appendBe32(picture, 500);
        appendBe32(picture, 500);
        appendBe32(picture, 24);
        appendBe32(picture, 0);
        const int dataBytes = std::max(0, options.pictureBytes - static_cast<int>(picture.size()) - 4);
        appendBe32(picture, static_cast<quint32>(dataBytes));
        picture.append(randomBytes(dataBytes, 5));
        QByteArray header;
        header.append('\x06');
        const auto length = static_cast<quint32>(picture.size());
        header.append(static_cast<char>((length >> 16U) & 0xFFU));
        header.append(static_cast<char>((length >> 8U) & 0xFFU));
        header.append(static_cast<char>(length & 0xFFU));
        flac.insert(42, header + picture); // "fLaC" (4) + STREAMINFO header (4) + body (34)
    }
    if (options.id3v2Bytes > 0) {
        flac.prepend(id3v2Tag(options.id3v2Bytes, 13));
    }
    return flac;
}

QByteArray makeM4a(const Pcm &pcm, M4aOptions options)
{
    FfmpegFile spec;
    spec.muxer = "ipod";
    spec.extension = "m4a";
    if (options.alac) {
        spec.codec = AV_CODEC_ID_ALAC;
        spec.format = AV_SAMPLE_FMT_S16P;
        spec.bitsPerRawSample = 16;
    } else {
        spec.codec = AV_CODEC_ID_AAC;
        spec.format = AV_SAMPLE_FMT_FLTP;
        spec.bitRate = 192000;
    }
    spec.movflags = options.fastStart ? "+faststart" : nullptr;
    return encodeWithFfmpeg(pcm, spec);
}

QByteArray makeOggFlac(const Pcm &pcm)
{
    FfmpegFile spec;
    spec.muxer = "ogg";
    spec.extension = "oga";
    spec.codec = AV_CODEC_ID_FLAC;
    spec.format = AV_SAMPLE_FMT_S16;
    spec.bitsPerRawSample = 16;
    return encodeWithFfmpeg(pcm, spec);
}

QByteArray makeOpus(const Pcm &pcm, int bitrateKbps)
{
    FfmpegFile spec;
    spec.muxer = "ogg";
    spec.extension = "opus";
    spec.codec = AV_CODEC_ID_OPUS;
    spec.format = AV_SAMPLE_FMT_FLTP;
    spec.bitRate = qint64{bitrateKbps} * 1000;
    return encodeWithFfmpeg(pcm, spec);
}

QByteArray makeOggVorbis(const Pcm &pcm, int bitrateKbps)
{
    FfmpegFile spec;
    spec.muxer = "ogg";
    spec.extension = "ogg";
    spec.codec = AV_CODEC_ID_VORBIS;
    spec.format = AV_SAMPLE_FMT_FLTP;
    spec.bitRate = qint64{bitrateKbps} * 1000;
    return encodeWithFfmpeg(pcm, spec);
}

double rmsDifference(const Pcm &a, const Pcm &b, qint64 skip)
{
    const qint64 frames = std::min(a.frames(), b.frames());
    const int channels = std::min(a.channels, b.channels);
    double sum = 0;
    qint64 count = 0;
    for (qint64 i = skip; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const double d = static_cast<double>(a.at(i, c)) - b.at(i, c);
            sum += d * d;
            ++count;
        }
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

double rms(const Pcm &pcm)
{
    double sum = 0;
    for (const float s : pcm.samples) {
        sum += static_cast<double>(s) * s;
    }
    return pcm.samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(pcm.samples.size()));
}

} // namespace selecta::testing
