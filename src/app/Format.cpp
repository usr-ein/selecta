// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Format.h"

#include "core/GetTracks.h"
#include "meta/Release.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::app::format {

QString length(std::optional<qint64> milliseconds)
{
    if (!milliseconds || *milliseconds <= 0) {
        return {};
    }
    const qint64 total = (*milliseconds + 500) / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total % 3600) / 60;
    const qint64 seconds = total % 60;
    if (hours > 0) {
        return u"%1:%2:%3"_s.arg(hours).arg(minutes, 2, 10, u'0').arg(seconds, 2, 10, u'0');
    }
    return u"%1:%2"_s.arg(minutes).arg(seconds, 2, 10, u'0');
}

QString clock(double seconds, int decimals)
{
    const double clamped = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    const double scale = decimals > 0 ? std::pow(10.0, decimals) : 1.0;
    const auto units = static_cast<qint64>(std::floor(clamped * scale)); // floor: a playhead never reads ahead
    const auto whole = static_cast<qint64>(static_cast<double>(units) / scale);
    QString text = u"%1:%2"_s.arg(whole / 60).arg(whole % 60, 2, 10, u'0');
    if (decimals > 0) {
        text += u'.' + u"%1"_s.arg(units % static_cast<qint64>(scale), decimals, 10, u'0');
    }
    return text;
}

QString quality(const core::FileChoice &file)
{
    QString format = file.extension.isEmpty() ? u"?"_s : file.extension.toUpper();
    const bool lossless = file.extension == u"flac" || file.extension == u"wav" || file.extension == u"aiff" ||
                          file.extension == u"aif" || file.extension == u"alac";
    if (lossless && file.bitDepth && file.sampleRate) {
        return u"%1 %2/%3"_s.arg(format).arg(*file.bitDepth).arg(*file.sampleRate / 1000.0, 0, 'g', 3);
    }
    if (file.bitrate) {
        if (file.vbr.value_or(false)) {
            return u"%1 VBR %2"_s.arg(format).arg(*file.bitrate);
        }
        return u"%1 %2"_s.arg(format).arg(*file.bitrate);
    }
    return format;
}

QString release(const meta::Release &release)
{
    QStringList parts;
    if (!release.title.isEmpty()) {
        parts.append(release.title);
    }
    if (!release.artists.isEmpty()) {
        parts.append(release.artist());
    }
    if (const QString time = length(release.durationMs); !time.isEmpty()) {
        parts.append(time);
    }
    return parts.join(u" · "_s);
}

QString lengthDelta(qint64 deltaSeconds)
{
    if (deltaSeconds == 0) {
        return u"±0 s"_s;
    }
    return deltaSeconds > 0 ? u"+%1 s"_s.arg(deltaSeconds) : u"−%1 s"_s.arg(-deltaSeconds);
}

QString size(qint64 bytes)
{
    constexpr double kKilo = 1000.0;
    const auto value = static_cast<double>(std::max<qint64>(bytes, 0));
    if (value < kKilo) {
        return u"%1 B"_s.arg(bytes);
    }
    if (value < kKilo * kKilo) {
        return u"%1 KB"_s.arg(std::round(value / kKilo), 0, 'f', 0);
    }
    if (value < kKilo * kKilo * kKilo) {
        return u"%1 MB"_s.arg(value / (kKilo * kKilo), 0, 'f', 1);
    }
    return u"%1 GB"_s.arg(value / (kKilo * kKilo * kKilo), 0, 'f', 2);
}

QString speed(qint64 bytesPerSecond)
{
    if (bytesPerSecond <= 0) {
        return {};
    }
    return size(bytesPerSecond) + u"/s"_s;
}

QString ago(qint64 thenMs, qint64 nowMs)
{
    const qint64 minutes = std::max<qint64>(0, nowMs - thenMs) / 60'000;
    if (minutes < 1) {
        return QCoreApplication::translate("selecta::app::format", "just now");
    }
    if (minutes < 60) {
        return QCoreApplication::translate("selecta::app::format", "%1 min ago").arg(minutes);
    }
    const qint64 hours = minutes / 60;
    if (hours < 48) {
        return QCoreApplication::translate("selecta::app::format", "%1 h ago").arg(hours);
    }
    return QCoreApplication::translate("selecta::app::format", "%1 days ago").arg(hours / 24);
}

QString about(qint64 milliseconds)
{
    constexpr qint64 kMinute = 60'000;
    if (milliseconds < kMinute) {
        return QCoreApplication::translate("selecta::app::format", "under a minute");
    }
    const qint64 minutes = (milliseconds + kMinute - 1) / kMinute;
    if (minutes < 60) {
        return QCoreApplication::translate("selecta::app::format", "about %1 min").arg(minutes);
    }
    const qint64 rest = minutes % 60;
    return rest == 0
               ? QCoreApplication::translate("selecta::app::format", "about %1 h").arg(minutes / 60)
               : QCoreApplication::translate("selecta::app::format", "about %1 h %2 min").arg(minutes / 60).arg(rest);
}

} // namespace selecta::app::format
