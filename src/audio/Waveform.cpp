// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/Waveform.h"

#include <algorithm>
#include <cmath>

namespace selecta::audio {

quint8 compandAmplitude(float amplitude)
{
    if (!(amplitude > 0.0F)) {
        return 0; // zero, negative and NaN
    }
    const float clamped = std::min(amplitude, 1.0F);
    return static_cast<quint8>(std::lround(255.0F * std::sqrt(clamped)));
}

float expandAmplitude(quint8 code)
{
    const float x = static_cast<float>(code) / 255.0F;
    return x * x;
}

qint64 columnStartFrame(qint64 column, int sampleRate)
{
    return (column * sampleRate) / kColumnsPerSecond;
}

qint64 columnCountForFrames(qint64 frames, int sampleRate)
{
    if (frames <= 0 || sampleRate <= 0) {
        return 0;
    }
    return ((frames * kColumnsPerSecond) + sampleRate - 1) / sampleRate;
}

Waveform Waveform::fromDetail(int sampleRate, qint64 frameCount, QList<WaveformColumn> detail, bool complete)
{
    Waveform w;
    w.m_sampleRate = sampleRate;
    w.m_frameCount = frameCount;
    w.m_complete = complete;
    w.m_overview = deriveOverview(detail);
    w.m_detail = std::move(detail);
    return w;
}

Waveform Waveform::fromOverview(int sampleRate, qint64 frameCount, QList<OverviewColumn> overview)
{
    Waveform w;
    w.m_sampleRate = sampleRate;
    w.m_frameCount = frameCount;
    w.m_complete = true;
    w.m_overview = std::move(overview);
    return w;
}

double Waveform::durationSeconds() const
{
    return m_sampleRate > 0 ? static_cast<double>(m_frameCount) / m_sampleRate : 0.0;
}

namespace {

// Bucket i of `count` over `n` items: [i·n/count, (i+1)·n/count), at least one item, clamped to the input.
std::pair<qint64, qint64> bucket(qint64 i, qint64 count, qint64 n)
{
    const qint64 begin = std::min((i * n) / count, n - 1);
    const qint64 end = std::clamp(((i + 1) * n) / count, begin + 1, n);
    return {begin, end};
}

} // namespace

QList<OverviewColumn> deriveOverview(std::span<const WaveformColumn> detail)
{
    QList<OverviewColumn> overview;
    const auto n = static_cast<qint64>(detail.size());
    if (n == 0) {
        return overview;
    }
    overview.resize(kOverviewColumns);
    for (qint64 j = 0; j < kOverviewColumns; ++j) {
        const auto [begin, end] = bucket(j, kOverviewColumns, n);
        OverviewColumn &column = overview[j];
        for (qint64 k = begin; k < end; ++k) {
            const WaveformColumn &source = detail[static_cast<std::size_t>(k)];
            for (std::size_t b = 0; b < kBandCount; ++b) {
                column.peak.at(b) = std::max(column.peak.at(b), source.peak.at(b));
            }
        }
    }
    return overview;
}

QList<WaveformColumn> reduceColumns(std::span<const WaveformColumn> columns, int count)
{
    QList<WaveformColumn> reduced;
    const auto n = static_cast<qint64>(columns.size());
    if (count <= 0 || n == 0) {
        return reduced;
    }
    reduced.resize(count);
    for (qint64 i = 0; i < count; ++i) {
        const auto [begin, end] = bucket(i, count, n);
        WaveformColumn &out = reduced[i];
        std::array<int, kBandCount> rmsSum{};
        for (qint64 k = begin; k < end; ++k) {
            const WaveformColumn &source = columns[static_cast<std::size_t>(k)];
            for (std::size_t b = 0; b < kBandCount; ++b) {
                out.peak.at(b) = std::max(out.peak.at(b), source.peak.at(b));
                rmsSum.at(b) += source.rms.at(b);
            }
        }
        const qint64 items = end - begin;
        for (std::size_t b = 0; b < kBandCount; ++b) {
            out.rms.at(b) = static_cast<quint8>((rmsSum.at(b) + (items / 2)) / items);
        }
    }
    return reduced;
}

} // namespace selecta::audio
