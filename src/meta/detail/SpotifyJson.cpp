// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/SpotifyJson.h"

#include "meta/SpotifyLinks.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"

#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail::spotify {

namespace {

constexpr qint64 kMaxDurationMs = qint64{24} * 3600 * 1000;
constexpr int kMaxArtists = 50;

QStringList artistNames(const QJsonArray &array)
{
    QStringList names;
    for (const auto &value : array) {
        if (names.size() >= kMaxArtists) {
            break;
        }
        const QString name = json::text(value.toObject(), "name"_L1);
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

// external_urls.spotify when it is an open.spotify.com HTTPS link, else the canonical link built from the ID.
QUrl spotifyUrl(const QJsonObject &object, ::selecta::meta::spotify::LinkKind kind, const QString &id)
{
    QUrl url(json::text(json::object(object, "external_urls"_L1), "spotify"_L1, 512));
    if (url.isValid() && url.scheme() == u"https"_s && url.host() == u"open.spotify.com"_s) {
        return url;
    }
    if (id.isEmpty()) {
        return {};
    }
    return QUrl(::selecta::meta::spotify::webUrl({kind, id}));
}

QString validId(const QJsonObject &object)
{
    const QString id = json::text(object, "id"_L1, 64);
    return ::selecta::meta::spotify::isValidId(id) ? id : QString();
}

void applyAlbumFields(Release &release, const QJsonObject &album)
{
    release.album = json::text(album, "name"_L1);
    release.albumArtists = artistNames(json::array(album, "artists"_L1));
    release.albumType = json::text(album, "album_type"_L1, 32).toLower();
    const auto [date, precision] =
        releaseDate(json::text(album, "release_date"_L1, 32), json::text(album, "release_date_precision"_L1, 16));
    release.releaseDate = date;
    release.releaseDatePrecision = precision;
    release.covers = images(json::array(album, "images"_L1));
    release.spotifyAlbumId = validId(album);
    release.trackTotal = json::positiveInt(album, "total_tracks"_L1);
    const QJsonObject ids = json::object(album, "external_ids"_L1);
    release.upc = normalizedBarcode(json::text(ids, "upc"_L1, 32));
    if (release.upc.isEmpty()) {
        release.upc = normalizedBarcode(json::text(ids, "ean"_L1, 32));
    }
}

} // namespace

QList<CoverRef> images(const QJsonArray &array)
{
    QList<CoverRef> covers;
    for (const auto &value : array) {
        if (covers.size() >= json::kMaxListItems) {
            break;
        }
        const QJsonObject image = value.toObject();
        const QUrl url(json::text(image, "url"_L1, 1024));
        if (!url.isValid() || url.scheme() != u"https"_s || url.host().isEmpty()) {
            continue;
        }
        CoverRef cover;
        cover.url = url;
        cover.width = json::positiveInt(image, "width"_L1).value_or(0);
        cover.height = json::positiveInt(image, "height"_L1).value_or(0);
        covers.append(cover);
    }
    std::ranges::stable_sort(covers, [](const CoverRef &a, const CoverRef &b) { return a.width > b.width; });
    return covers;
}

Release track(const QJsonObject &object)
{
    Release release;
    release.source = Source::Spotify;
    release.title = json::text(object, "name"_L1);
    release.artists = artistNames(json::array(object, "artists"_L1));
    const bool isLocal = json::boolean(object, "is_local"_L1).value_or(false);
    if (!isLocal) {
        release.spotifyId = validId(object);
        release.spotifyUrl = spotifyUrl(object, ::selecta::meta::spotify::LinkKind::Track, release.spotifyId);
    }
    if (const std::optional<qint64> duration = json::integer(object, "duration_ms"_L1);
        duration.has_value() && *duration > 0 && *duration < kMaxDurationMs) {
        release.durationMs = duration;
    }
    release.trackNumber = json::positiveInt(object, "track_number"_L1);
    release.discNumber = json::positiveInt(object, "disc_number"_L1);
    release.isrc = normalizedIsrc(json::text(json::object(object, "external_ids"_L1), "isrc"_L1, 32));
    const QJsonObject albumObject = json::object(object, "album"_L1);
    if (!albumObject.isEmpty()) {
        applyAlbumFields(release, albumObject);
    }
    return release;
}

Album album(const QJsonObject &object)
{
    Album result;
    result.id = validId(object);
    result.name = json::text(object, "name"_L1);
    result.artists = artistNames(json::array(object, "artists"_L1));
    result.albumType = json::text(object, "album_type"_L1, 32).toLower();
    const auto [date, precision] =
        releaseDate(json::text(object, "release_date"_L1, 32), json::text(object, "release_date_precision"_L1, 16));
    result.releaseDate = date;
    result.releaseDatePrecision = precision;
    result.totalTracks = json::positiveInt(object, "total_tracks"_L1).value_or(0);
    result.covers = images(json::array(object, "images"_L1));
    result.spotifyUrl = spotifyUrl(object, ::selecta::meta::spotify::LinkKind::Album, result.id);
    const QJsonObject ids = json::object(object, "external_ids"_L1);
    result.upc = normalizedBarcode(json::text(ids, "upc"_L1, 32));
    if (result.upc.isEmpty()) {
        result.upc = normalizedBarcode(json::text(ids, "ean"_L1, 32));
    }
    const QJsonArray copyrights = json::array(object, "copyrights"_L1);
    for (const auto &value : copyrights) {
        const QJsonObject copyright = value.toObject();
        if (json::text(copyright, "type"_L1, 4) == u"P"_s) {
            result.copyrightP = json::text(copyright, "text"_L1, 300);
            break;
        }
    }
    appendAlbumTracks(result, json::array(json::object(object, "tracks"_L1), "items"_L1));
    return result;
}

void appendAlbumTracks(Album &album, const QJsonArray &items)
{
    for (const auto &value : items) {
        if (album.tracks.size() >= SpotifyClient::kMaxImportTracks) {
            album.truncated = true;
            return;
        }
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) {
            continue;
        }
        album.tracks.append(track(object));
    }
}

void finishAlbum(Album &album)
{
    int discTotal = 0;
    for (const Release &release : std::as_const(album.tracks)) {
        discTotal = std::max(discTotal, release.discNumber.value_or(0));
    }
    album.discTotal = discTotal;
    for (Release &release : album.tracks) {
        release.album = album.name;
        release.albumArtists = album.artists;
        release.albumType = album.albumType;
        release.releaseDate = album.releaseDate;
        release.releaseDatePrecision = album.releaseDatePrecision;
        release.covers = album.covers;
        release.spotifyAlbumId = album.id;
        release.upc = album.upc;
        release.copyrightP = album.copyrightP;
        if (album.totalTracks > 0) {
            release.trackTotal = album.totalTracks;
        }
        if (discTotal > 0) {
            release.discTotal = discTotal;
        }
    }
}

PlaylistSummary playlist(const QJsonObject &object)
{
    PlaylistSummary summary;
    summary.id = validId(object);
    summary.name = json::text(object, "name"_L1);
    const QJsonObject owner = json::object(object, "owner"_L1);
    summary.owner = json::text(owner, "display_name"_L1);
    if (summary.owner.isEmpty()) {
        summary.owner = json::text(owner, "id"_L1, 128);
    }
    summary.collaborative = json::boolean(object, "collaborative"_L1).value_or(false);
    // The February 2026 migration renamed "tracks" to "items" on playlist objects; read either.
    QJsonObject counts = json::object(object, "items"_L1);
    if (counts.isEmpty()) {
        counts = json::object(object, "tracks"_L1);
    }
    summary.trackCount =
        static_cast<int>(std::clamp<qint64>(json::integer(counts, "total"_L1).value_or(0), 0, 1'000'000));
    summary.spotifyUrl = spotifyUrl(object, ::selecta::meta::spotify::LinkKind::Playlist, summary.id);
    return summary;
}

std::optional<Release> itemTrack(const QJsonObject &item)
{
    QJsonObject object = json::object(item, "track"_L1);
    if (object.isEmpty()) {
        object = json::object(item, "item"_L1);
    }
    if (object.isEmpty()) {
        return std::nullopt;
    }
    const QString type = json::text(object, "type"_L1, 16);
    if (!type.isEmpty() && type != u"track"_s) {
        return std::nullopt; // podcast episodes and anything else
    }
    if (json::boolean(item, "is_local"_L1).value_or(false)) {
        object.insert("is_local"_L1, true);
    }
    Release release = track(object);
    if (release.title.isEmpty()) {
        return std::nullopt;
    }
    return release;
}

bool isDevelopmentModeEndpoint(const QString &path)
{
    static const std::array<QRegularExpression, 8> kAllowed = {
        QRegularExpression(u"^/search$"_s),
        QRegularExpression(u"^/tracks/[0-9A-Za-z]{22}$"_s),
        QRegularExpression(u"^/albums/[0-9A-Za-z]{22}$"_s),
        QRegularExpression(u"^/albums/[0-9A-Za-z]{22}/tracks$"_s),
        QRegularExpression(u"^/me/playlists$"_s),
        QRegularExpression(u"^/playlists/[0-9A-Za-z]{22}$"_s), // metadata (the name)
        QRegularExpression(u"^/playlists/[0-9A-Za-z]{22}/items$"_s),
        QRegularExpression(u"^/me/tracks$"_s),
    };
    return std::ranges::any_of(kAllowed, [&](const QRegularExpression &re) { return re.match(path).hasMatch(); });
}

} // namespace selecta::meta::detail::spotify
