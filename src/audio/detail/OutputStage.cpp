// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/OutputStage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <soxr.h>

using namespace Qt::StringLiterals;

namespace selecta::audio::detail {
namespace {

constexpr float kMinus3dB = 0.70710678F;

// Left and right weights of each source channel in WAV/SMPTE order for common counts (FL FR FC LFE BL BR …).
struct Downmix
{
    std::array<float, 8> left{};
    std::array<float, 8> right{};
};

Downmix downmixFor(int channels)
{
    Downmix d;
    const float c = kMinus3dB;
    switch (channels) {
    case 3: // FL FR FC
        d.left = {1, 0, c};
        d.right = {0, 1, c};
        break;
    case 4: // FL FR BL BR
        d.left = {1, 0, c, 0};
        d.right = {0, 1, 0, c};
        break;
    case 5: // FL FR FC BL BR
        d.left = {1, 0, c, c, 0};
        d.right = {0, 1, c, 0, c};
        break;
    case 6: // FL FR FC LFE BL BR
        d.left = {1, 0, c, 0, c, 0};
        d.right = {0, 1, c, 0, 0, c};
        break;
    case 7: // FL FR FC LFE BC SL SR
        d.left = {1, 0, c, 0, c * c, c, 0};
        d.right = {0, 1, c, 0, c * c, 0, c};
        break;
    case 8: // FL FR FC LFE BL BR SL SR
        d.left = {1, 0, c, 0, c, 0, c, 0};
        d.right = {0, 1, c, 0, 0, c, 0, c};
        break;
    default:
        break;
    }
    // Normalise so a full-scale signal on every channel can't exceed full scale.
    float sumLeft = 0;
    float sumRight = 0;
    for (int i = 0; i < 8; ++i) {
        sumLeft += d.left.at(static_cast<std::size_t>(i));
        sumRight += d.right.at(static_cast<std::size_t>(i));
    }
    const float norm = std::max(sumLeft, sumRight);
    if (norm > 0) {
        for (std::size_t i = 0; i < 8; ++i) {
            d.left.at(i) /= norm;
            d.right.at(i) /= norm;
        }
    }
    return d;
}

} // namespace

OutputStage::~OutputStage()
{
    if (m_resampler != nullptr) {
        soxr_delete(m_resampler);
    }
}

Result<void> OutputStage::configure(int sourceRate, int sourceChannels, int outRate, int outChannels,
                                    ResamplerQuality quality)
{
    if (m_resampler != nullptr) {
        soxr_delete(m_resampler);
        m_resampler = nullptr;
    }
    if (sourceRate <= 0 || sourceChannels <= 0 || outRate <= 0 ||
        (outChannels != 1 && outChannels != 2 && outChannels != sourceChannels)) {
        return makeError(ErrorCode::InvalidArgument, u"invalid output configuration"_s);
    }
    m_sourceRate = sourceRate;
    m_sourceChannels = sourceChannels;
    m_outRate = outRate;
    m_outChannels = outChannels;
    reset();
    if (sourceRate != outRate) {
        soxr_error_t error = nullptr;
        const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        const soxr_quality_spec_t q = soxr_quality_spec(quality == ResamplerQuality::VeryHigh ? SOXR_VHQ : SOXR_HQ, 0);
        const soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
        m_resampler = soxr_create(sourceRate, outRate, static_cast<unsigned>(outChannels), &error, &io, &q, &runtime);
        if (m_resampler == nullptr || error != nullptr) {
            const QString why = error != nullptr ? QString::fromUtf8(error) : u"unknown error"_s;
            if (m_resampler != nullptr) {
                soxr_delete(m_resampler);
                m_resampler = nullptr;
            }
            return makeError(ErrorCode::Internal, u"can't create the resampler: %1"_s.arg(why));
        }
    }
    return {};
}

void OutputStage::reset()
{
    m_fifo.clear();
    m_readFrame = 0;
    if (m_resampler != nullptr) {
        soxr_clear(m_resampler);
    }
}

void OutputStage::mapChannels(const float *in, qint64 count, std::vector<float> &out) const
{
    out.resize(static_cast<std::size_t>(count * m_outChannels));
    const int sc = m_sourceChannels;
    if (m_outChannels == sc) {
        std::memcpy(out.data(), in, out.size() * sizeof(float));
        return;
    }
    if (sc == 1) { // mono → stereo
        for (qint64 i = 0; i < count; ++i) {
            out[static_cast<std::size_t>(2 * i)] = in[i];
            out[static_cast<std::size_t>((2 * i) + 1)] = in[i];
        }
        return;
    }
    if (sc == 2) { // stereo → mono
        for (qint64 i = 0; i < count; ++i) {
            out[static_cast<std::size_t>(i)] = 0.5F * (in[2 * i] + in[(2 * i) + 1]);
        }
        return;
    }
    // Surround → stereo (→ mono).
    const Downmix d = downmixFor(sc);
    const bool known = sc <= 8 && (d.left[0] > 0 || d.right[1] > 0);
    for (qint64 i = 0; i < count; ++i) {
        const float *frame = in + (i * sc);
        float left = 0;
        float right = 0;
        if (known) {
            for (int c = 0; c < sc; ++c) {
                left += frame[c] * d.left.at(static_cast<std::size_t>(c));
                right += frame[c] * d.right.at(static_cast<std::size_t>(c));
            }
        } else {
            for (int c = 0; c < sc; ++c) {
                left += frame[c];
            }
            left /= static_cast<float>(sc);
            right = left;
        }
        if (m_outChannels == 1) {
            out[static_cast<std::size_t>(i)] = 0.5F * (left + right);
        } else {
            out[static_cast<std::size_t>(2 * i)] = left;
            out[static_cast<std::size_t>((2 * i) + 1)] = right;
        }
    }
}

void OutputStage::append(const float *frames, qint64 count)
{
    if (m_readFrame > 0 && m_readFrame * 2 >= static_cast<qint64>(m_fifo.size()) / m_outChannels) {
        // Compact: drop what has been read once it is at least half the buffer.
        m_fifo.erase(m_fifo.begin(), m_fifo.begin() + static_cast<std::ptrdiff_t>(m_readFrame * m_outChannels));
        m_readFrame = 0;
    }
    m_fifo.insert(m_fifo.end(), frames, frames + (count * m_outChannels));
}

Result<void> OutputStage::resample(const float *in, qint64 count)
{
    // Output room: the exact ratio plus slack for the filter's tail.
    const auto room = static_cast<std::size_t>((static_cast<double>(count) * m_outRate / m_sourceRate) + 256.0);
    m_resampled.resize(room * static_cast<std::size_t>(m_outChannels));
    qint64 consumed = 0;
    while (true) {
        size_t inputDone = 0;
        size_t outputDone = 0;
        const soxr_error_t error = soxr_process(m_resampler, in == nullptr ? nullptr : in + (consumed * m_outChannels),
                                                in == nullptr ? 0 : static_cast<size_t>(count - consumed), &inputDone,
                                                m_resampled.data(), room, &outputDone);
        if (error != nullptr) {
            return makeError(ErrorCode::Internal, u"resampler failed: %1"_s.arg(QString::fromUtf8(error)));
        }
        append(m_resampled.data(), static_cast<qint64>(outputDone));
        consumed += static_cast<qint64>(inputDone);
        if (in == nullptr) {
            if (outputDone == 0) {
                break; // the tail is flushed
            }
        } else if (consumed >= count && outputDone < room) {
            break;
        }
    }
    return {};
}

Result<void> OutputStage::push(const float *frames, qint64 count)
{
    if (count <= 0) {
        return {};
    }
    const float *mapped = frames;
    if (m_outChannels != m_sourceChannels) {
        mapChannels(frames, count, m_mapped);
        mapped = m_mapped.data();
    }
    if (m_resampler == nullptr) {
        append(mapped, count);
        return {};
    }
    return resample(mapped, count);
}

Result<void> OutputStage::finish()
{
    if (m_resampler == nullptr) {
        return {};
    }
    return resample(nullptr, 0);
}

qint64 OutputStage::pull(float *destination, qint64 maxFrames)
{
    const qint64 count = std::min(maxFrames, buffered());
    if (count <= 0) {
        return 0;
    }
    std::memcpy(destination, m_fifo.data() + (m_readFrame * m_outChannels),
                static_cast<std::size_t>(count * m_outChannels) * sizeof(float));
    m_readFrame += count;
    if (m_readFrame * m_outChannels == static_cast<qint64>(m_fifo.size())) {
        m_fifo.clear();
        m_readFrame = 0;
    }
    return count;
}

} // namespace selecta::audio::detail
