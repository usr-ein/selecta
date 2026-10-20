// SPDX-License-Identifier: GPL-3.0-or-later
// A plain RIFF/WAVE writer: WAVE_FORMAT_PCM, never WAVE_FORMAT_EXTENSIBLE (conversion §2.3).
#include "convert/MetadataModel.h"
#include "convert/detail/Encoder.h"
#include "convert/detail/Quantizer.h"

#include <limits>
#include <string_view>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::convert::detail {
namespace {

constexpr qint64 kHeaderBytes = 44;
constexpr quint16 kWaveFormatPcm = 1;
// RIFF sizes are 32-bit; TagLib appends LIST/INFO and ID3 chunks later, so leave room for them.
constexpr qint64 kMaxDataBytes = qint64{std::numeric_limits<quint32>::max()} - (qint64{64} * 1024 * 1024);

void putLe16(std::vector<char> &out, quint32 value)
{
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void putLe32(std::vector<char> &out, quint32 value)
{
    putLe16(out, value & 0xFFFFU);
    putLe16(out, value >> 16U);
}

void putTag(std::vector<char> &out, std::string_view tag)
{
    out.insert(out.end(), tag.begin(), tag.end());
}

class WavEncoder final : public Encoder
{
public:
    WavEncoder(Sink &sink, EncoderFormat pcm, qint64 start)
        : m_sink(sink)
        , m_pcm(pcm)
        , m_quantizer(pcm.bitDepth, pcm.dither)
        , m_start(start)
    {}

    Result<void> writeHeader() { return m_sink.write(header(0).data(), kHeaderBytes); }

    Result<void> write(const float *samples, qint64 frames) override
    {
        const auto count = static_cast<std::size_t>(frames * m_pcm.channels);
        m_quantized.resize(count);
        m_quantizer.quantize({samples, count}, m_quantized);
        const auto bytesPerSample = static_cast<std::size_t>(m_pcm.bitDepth / 8);
        m_bytes.resize(count * bytesPerSample);
        std::size_t at = 0;
        for (const qint32 sample : m_quantized) {
            const auto bits = static_cast<quint32>(sample);
            m_bytes[at++] = static_cast<char>(bits & 0xFFU);
            m_bytes[at++] = static_cast<char>((bits >> 8U) & 0xFFU);
            if (bytesPerSample == 3) {
                m_bytes[at++] = static_cast<char>((bits >> 16U) & 0xFFU);
            }
        }
        const auto size = static_cast<qint64>(m_bytes.size());
        if (m_dataBytes + size > kMaxDataBytes) {
            return makeError(ErrorCode::TooLarge, u"WAV files can't be larger than 4 GiB"_s);
        }
        if (const Result<void> written = m_sink.write(m_bytes.data(), size); !written.ok()) {
            return written;
        }
        m_dataBytes += size;
        return {};
    }

    Result<void> finish() override
    {
        // Chunks are padded to an even size; the pad byte isn't part of the data size.
        if (m_dataBytes % 2 != 0) {
            const char pad = 0;
            if (const Result<void> written = m_sink.write(&pad, 1); !written.ok()) {
                return written;
            }
        }
        const qint64 end = m_sink.position();
        const std::vector<char> patched = header(m_dataBytes);
        if (const Result<void> sought = m_sink.seek(m_start); !sought.ok()) {
            return sought;
        }
        if (const Result<void> written = m_sink.write(patched.data(), kHeaderBytes); !written.ok()) {
            return written;
        }
        return m_sink.seek(end);
    }

    qint64 clippedSamples() const override { return m_quantizer.clippedSamples(); }

private:
    std::vector<char> header(qint64 dataBytes) const
    {
        const auto channels = static_cast<quint32>(m_pcm.channels);
        const auto rate = static_cast<quint32>(m_pcm.sampleRate);
        const auto bits = static_cast<quint32>(m_pcm.bitDepth);
        const quint32 blockAlign = channels * (bits / 8);
        const auto data = static_cast<quint32>(dataBytes);
        const quint32 padded = data + (data % 2);
        std::vector<char> out;
        out.reserve(kHeaderBytes);
        putTag(out, "RIFF");
        putLe32(out, 4 + (8 + 16) + 8 + padded); // "WAVE" + fmt chunk + data chunk
        putTag(out, "WAVE");
        putTag(out, "fmt ");
        putLe32(out, 16);
        putLe16(out, kWaveFormatPcm);
        putLe16(out, channels);
        putLe32(out, rate);
        putLe32(out, rate * blockAlign);
        putLe16(out, blockAlign);
        putLe16(out, bits);
        putTag(out, "data");
        putLe32(out, data);
        return out;
    }

    Sink &m_sink;
    EncoderFormat m_pcm;
    Quantizer m_quantizer;
    qint64 m_start = 0;
    qint64 m_dataBytes = 0;
    std::vector<qint32> m_quantized;
    std::vector<char> m_bytes;
};

} // namespace

Result<std::unique_ptr<Encoder>> createWavEncoder(EncoderFormat pcm, Sink &sink)
{
    if (pcm.bitDepth != 16 && pcm.bitDepth != 24) {
        return makeError(ErrorCode::InvalidArgument, u"WAV output is 16- or 24-bit, not %1-bit"_s.arg(pcm.bitDepth));
    }
    // WAVE_FORMAT_PCM is defined for mono and stereo; more channels need WAVE_FORMAT_EXTENSIBLE.
    if (pcm.channels != 1 && pcm.channels != 2) {
        return makeError(ErrorCode::InvalidArgument,
                         u"WAV output is mono or stereo, not %1 channels"_s.arg(pcm.channels));
    }
    if (pcm.sampleRate < 1) {
        return makeError(ErrorCode::InvalidArgument, u"no sample rate"_s);
    }
    auto encoder = std::make_unique<WavEncoder>(sink, pcm, sink.position());
    if (const Result<void> written = encoder->writeHeader(); !written.ok()) {
        return written.error();
    }
    return std::unique_ptr<Encoder>(std::move(encoder));
}

Result<std::unique_ptr<Encoder>> createEncoder(const EncodeSettings &settings, EncoderFormat pcm, Sink &sink)
{
    switch (settings.format) {
    case OutputFormat::Mp3:
        return createMp3Encoder(settings.mp3, pcm, sink);
    case OutputFormat::Wav:
        return createWavEncoder(pcm, sink);
    case OutputFormat::Aiff:
    case OutputFormat::Flac:
    case OutputFormat::Alac:
        return createFfmpegEncoder(settings.format, pcm, sink);
    case OutputFormat::KeepAudio:
        break;
    }
    return makeError(ErrorCode::InvalidArgument, u"keeping the audio involves no encoder"_s);
}

} // namespace selecta::convert::detail
