// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Decoder.h"
#include "audio/Error.h"

#include <QtGlobal>

#include <vector>

struct soxr;

namespace selecta::audio::detail {

// The last step of decoding: maps the source channels to the requested layout, resamples with libsoxr when the rates
// differ, and buffers the result for reads.
//
// Channel mapping: mono = (L+R)/2 (or the mean of all channels beyond stereo, after a standard downmix); stereo
// duplicates mono and downmixes surround with the usual -3 dB centre and surround coefficients, normalised so it can't
// clip. LFE is dropped.
//
// Threading: one thread at a time.
class OutputStage
{
public:
    OutputStage() = default;
    ~OutputStage();
    OutputStage(const OutputStage &) = delete;
    OutputStage &operator=(const OutputStage &) = delete;
    OutputStage(OutputStage &&) = delete;
    OutputStage &operator=(OutputStage &&) = delete;

    // `outChannels` is 1, 2, or equal to `sourceChannels`.
    Result<void> configure(int sourceRate, int sourceChannels, int outRate, int outChannels, ResamplerQuality quality);

    // Drops buffered audio and resets the resampler (after a seek).
    void reset();
    // Adds `frames` interleaved frames of source audio.
    Result<void> push(const float *frames, qint64 count);
    // End of stream: flushes the resampler's tail.
    Result<void> finish();
    // Moves up to `maxFrames` frames into `destination`. Returns the count.
    qint64 pull(float *destination, qint64 maxFrames);
    qint64 buffered() const { return (static_cast<qint64>(m_fifo.size()) / m_outChannels) - m_readFrame; }

    int outChannels() const { return m_outChannels; }
    bool resamples() const { return m_resampler != nullptr; }

private:
    void mapChannels(const float *in, qint64 count, std::vector<float> &out) const;
    Result<void> resample(const float *in, qint64 count);
    void append(const float *frames, qint64 count);

    int m_sourceRate = 0;
    int m_sourceChannels = 0;
    int m_outRate = 0;
    int m_outChannels = 1;
    soxr *m_resampler = nullptr;
    std::vector<float> m_mapped;
    std::vector<float> m_resampled;
    std::vector<float> m_fifo; // interleaved, m_outChannels per frame
    qint64 m_readFrame = 0;    // frames of m_fifo already pulled
};

} // namespace selecta::audio::detail
