// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Format.h"

#include "core/GetTracks.h"
#include "meta/Release.h"

#include <QStringList>

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

} // namespace selecta::app::format
