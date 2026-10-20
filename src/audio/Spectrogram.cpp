// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/Spectrogram.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::audio {
namespace {

constexpr int kMaxSampleRate = 768000;
constexpr double kStepsPerDb = 255.0 / (kSpectrogramCeilingDb - kSpectrogramFloorDb);

} // namespace

Result<Spectrogram> Spectrogram::fromLevels(int sampleRate, qint64 frameCount, int columns, int bins, QByteArray levels)
{
    if (sampleRate <= 0 || sampleRate > kMaxSampleRate) {
        return makeError(ErrorCode::InvalidArgument, u"spectrogram sample rate %1 out of range"_s.arg(sampleRate));
    }
    if (bins < 1 || bins > kSpectrogramMaxBins || columns < 0 || columns > kSpectrogramMaxColumns ||
        frameCount < columns) {
        return makeError(
            ErrorCode::InvalidArgument,
            u"spectrogram of %1 × %2 over %3 frames out of range"_s.arg(columns).arg(bins).arg(frameCount));
    }
    if (levels.size() != qsizetype{columns} * bins) {
        return makeError(ErrorCode::InvalidArgument, u"spectrogram levels of the wrong size"_s);
    }
    Spectrogram s;
    s.m_sampleRate = sampleRate;
    s.m_frameCount = frameCount;
    s.m_columns = columns;
    s.m_bins = bins;
    s.m_levels = std::move(levels);
    return s;
}

double Spectrogram::durationSeconds() const
{
    return m_sampleRate > 0 ? static_cast<double>(m_frameCount) / m_sampleRate : 0.0;
}

double Spectrogram::binLowHz(int bin) const
{
    return m_bins > 0 ? (static_cast<double>(bin) * nyquistHz()) / m_bins : 0.0;
}

int Spectrogram::binForFrequency(double hz) const
{
    if (m_bins <= 0 || m_sampleRate <= 0 || !(hz > 0.0)) {
        return 0;
    }
    const double bin = std::floor((hz * m_bins) / nyquistHz());
    return static_cast<int>(std::clamp(bin, 0.0, static_cast<double>(m_bins - 1)));
}

qint64 Spectrogram::columnStartFrame(int column) const
{
    if (m_columns <= 0) {
        return 0;
    }
    const qint64 c = std::clamp<qint64>(column, 0, m_columns);
    return (c * m_frameCount) / m_columns;
}

quint8 Spectrogram::level(int column, int bin) const
{
    if (column < 0 || column >= m_columns || bin < 0 || bin >= m_bins) {
        return 0;
    }
    return static_cast<quint8>(m_levels[(qsizetype{column} * m_bins) + bin]);
}

std::span<const quint8> Spectrogram::column(int column) const
{
    if (column < 0 || column >= m_columns) {
        return {};
    }
    const auto *first = reinterpret_cast<const quint8 *>(m_levels.constData()) + (qsizetype{column} * m_bins);
    return {first, static_cast<std::size_t>(m_bins)};
}

quint8 Spectrogram::levelForDb(double db)
{
    if (!(db > kSpectrogramFloorDb)) {
        return 0; // at or below the floor, and NaN
    }
    const double steps = std::min((db - kSpectrogramFloorDb) * kStepsPerDb, 255.0);
    return static_cast<quint8>(std::lround(steps));
}

double Spectrogram::dbForLevel(quint8 level)
{
    return kSpectrogramFloorDb + (level / kStepsPerDb);
}

} // namespace selecta::audio
