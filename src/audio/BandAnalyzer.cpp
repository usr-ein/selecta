// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/BandAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace selecta::audio {
namespace {

constexpr int kMinRate = 8000;
constexpr int kMaxRate = 768000;
constexpr double kButterworthQ = std::numbers::sqrt2 / 2.0;
constexpr double kDenormalFloor = 1e-30;

enum class Pass { Low, High };

// RBJ cookbook biquad (Butterworth Q), normalised so that a0 = 1.
void design(double frequency, double sampleRate, Pass pass, double &b0, double &b1, double &b2, double &a1, double &a2)
{
    const double w0 = 2.0 * std::numbers::pi * frequency / sampleRate;
    const double cosine = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kButterworthQ);
    const double a0 = 1.0 + alpha;
    if (pass == Pass::Low) {
        b0 = (1.0 - cosine) / 2.0 / a0;
        b1 = (1.0 - cosine) / a0;
    } else {
        b0 = (1.0 + cosine) / 2.0 / a0;
        b1 = -(1.0 + cosine) / a0;
    }
    b2 = b0;
    a1 = -2.0 * cosine / a0;
    a2 = (1.0 - alpha) / a0;
}

template <typename Biquad>
void configure(std::array<Biquad, 2> &stages, double frequency, double sampleRate, Pass pass)
{
    for (Biquad &stage : stages) {
        design(frequency, sampleRate, pass, stage.b0, stage.b1, stage.b2, stage.a1, stage.a2);
        stage.z1 = 0;
        stage.z2 = 0;
    }
}

template <typename Biquad>
double run(std::array<Biquad, 2> &stages, double x)
{
    for (Biquad &s : stages) {
        const double y = (s.b0 * x) + s.z1;
        s.z1 = (s.b1 * x) - (s.a1 * y) + s.z2;
        s.z2 = (s.b2 * x) - (s.a2 * y);
        x = y;
    }
    return x;
}

template <typename Biquad>
void flushDenormals(std::array<Biquad, 2> &stages)
{
    for (Biquad &s : stages) {
        if (std::abs(s.z1) < kDenormalFloor) {
            s.z1 = 0;
        }
        if (std::abs(s.z2) < kDenormalFloor) {
            s.z2 = 0;
        }
    }
}

} // namespace

BandAnalyzer::BandAnalyzer(int sampleRate, qint64 firstColumn)
    : m_sampleRate(std::clamp(sampleRate, kMinRate, kMaxRate))
    , m_column(std::max<qint64>(0, firstColumn))
    , m_columnEnd(columnStartFrame(m_column + 1, m_sampleRate))
    , m_position(columnStartFrame(m_column, m_sampleRate))
{
    const double rate = m_sampleRate;
    configure(m_lowPass1, kLowMidCrossoverHz, rate, Pass::Low);
    configure(m_highPass1, kLowMidCrossoverHz, rate, Pass::High);
    configure(m_lowPass2, kMidHighCrossoverHz, rate, Pass::Low);
    configure(m_highPass2, kMidHighCrossoverHz, rate, Pass::High);
}

qint64 BandAnalyzer::warmUpFrames(int sampleRate)
{
    return (static_cast<qint64>(std::clamp(sampleRate, kMinRate, kMaxRate)) * 80) / 1000;
}

void BandAnalyzer::filter(float sample, std::array<double, kBandCount> &bands)
{
    const double x = sample;
    bands[0] = run(m_lowPass1, x);
    const double rest = run(m_highPass1, x);
    bands[1] = run(m_lowPass2, rest);
    bands[2] = run(m_highPass2, rest);
}

void BandAnalyzer::warmUp(std::span<const float> samples)
{
    std::array<double, kBandCount> bands{};
    for (const float s : samples) {
        filter(s, bands);
    }
}

void BandAnalyzer::emitColumn(QList<WaveformColumn> &out)
{
    WaveformColumn column;
    const auto count = static_cast<double>(m_accumulator.count);
    for (std::size_t b = 0; b < kBandCount; ++b) {
        column.peak.at(b) = compandAmplitude(static_cast<float>(m_accumulator.peak.at(b)));
        const double rms = count > 0 ? std::sqrt(m_accumulator.sumSquares.at(b) / count) : 0.0;
        column.rms.at(b) = compandAmplitude(static_cast<float>(rms));
    }
    out.append(column);
    m_accumulator = {};
    ++m_column;
    m_columnEnd = columnStartFrame(m_column + 1, m_sampleRate);
    flushDenormals(m_lowPass1);
    flushDenormals(m_highPass1);
    flushDenormals(m_lowPass2);
    flushDenormals(m_highPass2);
}

void BandAnalyzer::process(std::span<const float> samples, QList<WaveformColumn> &out)
{
    std::array<double, kBandCount> bands{};
    for (const float s : samples) {
        filter(s, bands);
        for (std::size_t b = 0; b < kBandCount; ++b) {
            const double magnitude = std::abs(bands.at(b));
            m_accumulator.peak.at(b) = std::max(m_accumulator.peak.at(b), magnitude);
            m_accumulator.sumSquares.at(b) += bands.at(b) * bands.at(b);
        }
        ++m_accumulator.count;
        ++m_position;
        ++m_frames;
        if (m_position >= m_columnEnd) {
            emitColumn(out);
        }
    }
}

void BandAnalyzer::finish(QList<WaveformColumn> &out)
{
    if (m_accumulator.count > 0) {
        emitColumn(out);
        // The next column starts where this partial one stopped.
        m_position = columnStartFrame(m_column, m_sampleRate);
    }
}

} // namespace selecta::audio
