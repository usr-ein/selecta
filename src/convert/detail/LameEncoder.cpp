// SPDX-License-Identifier: GPL-3.0-or-later
// MP3 through libmp3lame directly, with the genuine LAME Info tag (conversion §2.1, §4.4).
#include "convert/MetadataModel.h"
#include "convert/detail/Encoder.h"

#include <lame/lame.h>
#include <limits>
#include <mutex>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::convert::detail {
namespace {

// FFmpeg marks its libmp3lame wrapper FF_CODEC_CAP_NOT_INIT_THREADSAFE: lame_init_params() fills global tables.
std::mutex &lameInitMutex()
{
    static std::mutex mutex;
    return mutex;
}

// Frames per lame_encode_buffer call; bounds the output buffer (LAME's worst case: 1.25 × samples + 7200 bytes).
constexpr int kChunkFrames = 4096;
constexpr int kOutputBytes = (kChunkFrames * 5 / 4) + 7200;
// lame_get_lametag_frame() needs room for one frame: 2880 bytes is the largest MPEG-1 Layer III frame.
constexpr std::size_t kTagFrameBytes = 2880;

class LameEncoder final : public Encoder
{
public:
    LameEncoder(lame_t lame, int channels, Sink &sink, qint64 streamStart)
        : m_lame(lame)
        , m_channels(channels)
        , m_sink(sink)
        , m_streamStart(streamStart)
        , m_buffer(kOutputBytes)
    {}

    ~LameEncoder() override
    {
        const std::scoped_lock lock(lameInitMutex());
        lame_close(m_lame);
    }

    LameEncoder(const LameEncoder &) = delete;
    LameEncoder &operator=(const LameEncoder &) = delete;
    LameEncoder(LameEncoder &&) = delete;
    LameEncoder &operator=(LameEncoder &&) = delete;

    Result<void> write(const float *samples, qint64 frames) override
    {
        while (frames > 0) {
            const int count = static_cast<int>(std::min<qint64>(frames, kChunkFrames));
            const int written =
                m_channels == 1
                    ? lame_encode_buffer_ieee_float(m_lame, samples, samples, count, m_buffer.data(), kOutputBytes)
                    : lame_encode_buffer_interleaved_ieee_float(m_lame, samples, count, m_buffer.data(), kOutputBytes);
            if (written < 0) {
                return makeError(ErrorCode::EncoderError, u"LAME refused the audio (error %1)"_s.arg(written));
            }
            if (const Result<void> stored = m_sink.write(m_buffer.data(), written); !stored.ok()) {
                return stored;
            }
            samples += qint64{count} * m_channels;
            frames -= count;
        }
        return {};
    }

    Result<void> finish() override
    {
        const int tail = lame_encode_flush(m_lame, m_buffer.data(), kOutputBytes);
        if (tail < 0) {
            return makeError(ErrorCode::EncoderError, u"LAME couldn't finish the stream (error %1)"_s.arg(tail));
        }
        if (const Result<void> stored = m_sink.write(m_buffer.data(), tail); !stored.ok()) {
            return stored;
        }
        // LAME began the stream with an empty frame; the final tag (frame count, delay, padding, CRCs) replaces it.
        std::vector<unsigned char> tag(kTagFrameBytes);
        const std::size_t size = lame_get_lametag_frame(m_lame, tag.data(), tag.size());
        if (size == 0 || size > tag.size()) {
            return makeError(ErrorCode::EncoderError, u"LAME didn't produce its Info tag"_s);
        }
        const qint64 end = m_sink.position();
        if (const Result<void> sought = m_sink.seek(m_streamStart); !sought.ok()) {
            return sought;
        }
        if (const Result<void> stored = m_sink.write(tag.data(), static_cast<qint64>(size)); !stored.ok()) {
            return stored;
        }
        return m_sink.seek(end);
    }

private:
    lame_t m_lame;
    int m_channels;
    Sink &m_sink;
    qint64 m_streamStart;
    std::vector<unsigned char> m_buffer;
};

bool isMpeg1Rate(int rate)
{
    return rate == 32000 || rate == 44100 || rate == 48000;
}

} // namespace

Result<std::unique_ptr<Encoder>> createMp3Encoder(Mp3Settings settings, EncoderFormat format, Sink &sink)
{
    if (!isMpeg1Rate(format.sampleRate)) {
        return makeError(ErrorCode::InvalidArgument,
                         u"MP3 output needs 32, 44.1 or 48 kHz, not %1 Hz"_s.arg(format.sampleRate));
    }
    if (format.channels != 1 && format.channels != 2) {
        return makeError(ErrorCode::InvalidArgument, u"MP3 holds one or two channels, not %1"_s.arg(format.channels));
    }
    lame_t lame = nullptr;
    {
        const std::scoped_lock lock(lameInitMutex());
        lame = lame_init();
        if (lame == nullptr) {
            return makeError(ErrorCode::EncoderError, u"LAME couldn't start"_s);
        }
        lame_set_num_channels(lame, format.channels);
        lame_set_in_samplerate(lame, format.sampleRate);
        lame_set_out_samplerate(lame, format.sampleRate); // never LAME's own resampler
        lame_set_mode(lame, format.channels == 1 ? MONO : JOINT_STEREO);
        lame_set_quality(lame, settings.algorithmQuality);
        if (settings.mode == Mp3Mode::Cbr) {
            lame_set_VBR(lame, vbr_off);
            lame_set_brate(lame, settings.bitrateKbps);
        } else {
            lame_set_VBR(lame, vbr_default);
            lame_set_VBR_q(lame, settings.vbrQuality);
        }
        lame_set_bWriteVbrTag(lame, 1);           // the Info/Xing frame, finalised in finish()
        lame_set_write_id3tag_automatic(lame, 0); // tags are TagLib's job; the stream starts with the tag frame
        if (lame_init_params(lame) < 0) {
            lame_close(lame);
            return makeError(ErrorCode::EncoderError,
                             u"LAME refused the settings %1"_s.arg(lameSettingsString(settings)));
        }
    }
    return std::unique_ptr<Encoder>(std::make_unique<LameEncoder>(lame, format.channels, sink, sink.position()));
}

} // namespace selecta::convert::detail
