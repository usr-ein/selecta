// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/DeezerJson.h"

#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail::deezer {

namespace {

constexpr qint64 kMaxDurationSec = qint64{24} * 3600;

// Deezer IDs are numbers (sometimes negative for user uploads): keep positive whole numbers only, as text.
QString idOf(const QJsonObject &object)
{
    const std::optional<qint64> id = json::integer(object, "id"_L1);
    if (!id.has_value() || *id <= 0) {
        return {};
    }
    return QString::number(*id);
}

QUrl httpsUrl(const QString &text, const QStringList &hostSuffixes)
{
    const QUrl url(text);
    if (!url.isValid() || url.scheme() != u"https"_s) {
        return {};
    }
    const QString host = url.host().toLower();
    const bool allowed = std::ranges::any_of(
        hostSuffixes, [&](const QString &suffix) { return host == suffix || host.endsWith(u'.' + suffix); });
    return allowed ? url : QUrl();
}

QList<CoverRef> covers(const QJsonObject &album)
{
    struct Size
    {
        QLatin1StringView key;
        int px = 0;
    };
    static constexpr std::array<Size, 4> kSizes = {Size{"cover_xl"_L1, 1000}, Size{"cover_big"_L1, 500},
                                                   Size{"cover_medium"_L1, 250}, Size{"cover_small"_L1, 56}};
    QList<CoverRef> list;
    for (const Size &size : kSizes) {
        const QUrl url = httpsUrl(json::text(album, size.key, 1024), {u"dzcdn.net"_s});
        if (url.isValid()) {
            list.append({url, size.px, size.px});
        }
    }
    return list;
}

} // namespace

std::optional<int> errorCode(const QJsonObject &object)
{
    const QJsonValue error = object.value("error"_L1);
    if (!error.isObject()) {
        return std::nullopt;
    }
    const std::optional<qint64> code = json::integer(error.toObject(), "code"_L1);
    if (!code.has_value() || *code < 0 || *code > std::numeric_limits<int>::max()) {
        return -1; // an error without a readable code
    }
    return static_cast<int>(*code);
}

std::optional<DeezerTrack> track(const QJsonObject &object)
{
    DeezerTrack track;
    Release &release = track.release;
    release.source = Source::Deezer;
    release.deezerId = idOf(object);
    release.title = json::text(object, "title"_L1);
    if (release.deezerId.isEmpty() || release.title.isEmpty()) {
        return std::nullopt;
    }
    if (const QString type = json::text(object, "type"_L1, 16); !type.isEmpty() && type != u"track"_s) {
        return std::nullopt;
    }
    track.titleVersion = json::text(object, "title_version"_L1, 200);
    release.deezerUrl = httpsUrl(json::text(object, "link"_L1, 512), {u"deezer.com"_s});

    // Contributors (full tracks) list every credited artist in order; search hits carry the main artist only.
    const QJsonArray contributors = json::array(object, "contributors"_L1);
    for (const auto &value : contributors) {
        if (release.artists.size() >= json::kMaxListItems) {
            break;
        }
        const QString name = json::text(value.toObject(), "name"_L1);
        if (!name.isEmpty() && !release.artists.contains(name)) {
            release.artists.append(name);
        }
    }
    if (release.artists.isEmpty()) {
        const QString main = json::text(json::object(object, "artist"_L1), "name"_L1);
        if (!main.isEmpty()) {
            release.artists.append(main);
        }
    }
    if (const std::optional<qint64> seconds = json::integer(object, "duration"_L1);
        seconds.has_value() && *seconds > 0 && *seconds < kMaxDurationSec) {
        release.durationMs = *seconds * 1000;
    }
    release.trackNumber = json::positiveInt(object, "track_position"_L1);
    release.discNumber = json::positiveInt(object, "disk_number"_L1);
    release.isrc = normalizedIsrc(json::text(object, "isrc"_L1, 32));

    const QJsonObject albumObject = json::object(object, "album"_L1);
    release.album = json::text(albumObject, "title"_L1);
    release.deezerAlbumId = idOf(albumObject);
    release.covers = covers(albumObject);
    QString date = json::text(object, "release_date"_L1, 32);
    if (date.isEmpty()) {
        date = json::text(albumObject, "release_date"_L1, 32);
    }
    const auto [releaseDate, precision] = detail::releaseDate(date);
    release.releaseDate = releaseDate;
    release.releaseDatePrecision = precision;

    track.previewUrl = httpsUrl(json::text(object, "preview"_L1, 2048), {u"dzcdn.net"_s, u"deezer.com"_s});
    if (const std::optional<double> bpm = json::number(object, "bpm"_L1); bpm.has_value() && *bpm > 0 && *bpm < 1000) {
        track.bpm = bpm;
    }
    return track;
}

QList<DeezerTrack> searchHits(const QJsonObject &object)
{
    QList<DeezerTrack> hits;
    const QJsonArray data = json::array(object, "data"_L1);
    for (const auto &value : data) {
        if (hits.size() >= kMaxSearchHits) {
            break;
        }
        if (std::optional<DeezerTrack> hit = track(value.toObject())) {
            hits.append(std::move(*hit));
        }
    }
    return hits;
}

std::optional<DeezerAlbum> album(const QJsonObject &object)
{
    DeezerAlbum album;
    album.id = idOf(object);
    album.title = json::text(object, "title"_L1);
    if (album.id.isEmpty() || album.title.isEmpty()) {
        return std::nullopt;
    }
    album.label = json::text(object, "label"_L1, 300);
    album.upc = normalizedBarcode(json::text(object, "upc"_L1, 32));
    const QJsonArray genres = json::array(json::object(object, "genres"_L1), "data"_L1);
    for (const auto &value : genres) {
        const QString name = json::text(value.toObject(), "name"_L1, 100);
        if (!name.isEmpty() && album.genres.size() < json::kMaxListItems) {
            album.genres.append(name);
        }
    }
    album.recordType = json::text(object, "record_type"_L1, 32).toLower();
    album.releaseDate = detail::releaseDate(json::text(object, "release_date"_L1, 32)).first;
    album.trackCount = json::positiveInt(object, "nb_tracks"_L1).value_or(0);
    album.covers = covers(object);
    album.url = httpsUrl(json::text(object, "link"_L1, 512), {u"deezer.com"_s});
    return album;
}

} // namespace selecta::meta::detail::deezer
