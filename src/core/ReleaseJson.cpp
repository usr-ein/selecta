// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ReleaseJson.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

QString sourceKey(meta::Source source)
{
    switch (source) {
    case meta::Source::Spotify:
        return u"spotify"_s;
    case meta::Source::Deezer:
        return u"deezer"_s;
    case meta::Source::MusicBrainz:
        return u"musicbrainz"_s;
    case meta::Source::CoverArtArchive:
        return u"coverartarchive"_s;
    case meta::Source::YouTube:
        return u"youtube"_s;
    }
    return u"spotify"_s;
}

std::optional<meta::Source> sourceFrom(const QString &key)
{
    for (const meta::Source source : {meta::Source::Spotify, meta::Source::Deezer, meta::Source::MusicBrainz,
                                      meta::Source::CoverArtArchive, meta::Source::YouTube}) {
        if (sourceKey(source) == key) {
            return source;
        }
    }
    return std::nullopt;
}

QString precisionKey(meta::DatePrecision precision)
{
    switch (precision) {
    case meta::DatePrecision::Year:
        return u"year"_s;
    case meta::DatePrecision::Month:
        return u"month"_s;
    case meta::DatePrecision::Day:
        return u"day"_s;
    case meta::DatePrecision::Unknown:
        break;
    }
    return u"unknown"_s;
}

meta::DatePrecision precisionFrom(const QString &key)
{
    if (key == u"year") {
        return meta::DatePrecision::Year;
    }
    if (key == u"month") {
        return meta::DatePrecision::Month;
    }
    if (key == u"day") {
        return meta::DatePrecision::Day;
    }
    return meta::DatePrecision::Unknown;
}

void putText(QJsonObject &object, const QString &key, const QString &value)
{
    if (!value.isEmpty()) {
        object.insert(key, value);
    }
}

void putList(QJsonObject &object, const QString &key, const QStringList &values)
{
    if (!values.isEmpty()) {
        object.insert(key, QJsonArray::fromStringList(values));
    }
}

template <typename T>
void putOptional(QJsonObject &object, const QString &key, const std::optional<T> &value)
{
    if (value) {
        object.insert(key, static_cast<qint64>(*value));
    }
}

QStringList list(const QJsonObject &object, const QString &key)
{
    QStringList values;
    const QJsonArray array = object.value(key).toArray();
    for (const auto &value : array) {
        if (value.isString()) {
            values.append(value.toString());
        }
    }
    return values;
}

std::optional<int> optionalInt(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? std::optional<int>(value.toInt()) : std::nullopt;
}

std::optional<qint64> optionalInt64(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isDouble() ? std::optional<qint64>(value.toInteger()) : std::nullopt;
}

} // namespace

QJsonObject releaseToJson(const meta::Release &release)
{
    QJsonObject object;
    object.insert(u"source"_s, sourceKey(release.source));
    putText(object, u"title"_s, release.title);
    putList(object, u"artists"_s, release.artists);
    putText(object, u"album"_s, release.album);
    putList(object, u"albumArtists"_s, release.albumArtists);
    putText(object, u"albumType"_s, release.albumType);
    putText(object, u"releaseDate"_s, release.releaseDate);
    object.insert(u"releaseDatePrecision"_s, precisionKey(release.releaseDatePrecision));
    putOptional(object, u"trackNumber"_s, release.trackNumber);
    putOptional(object, u"trackTotal"_s, release.trackTotal);
    putOptional(object, u"discNumber"_s, release.discNumber);
    putOptional(object, u"discTotal"_s, release.discTotal);
    putOptional(object, u"durationMs"_s, release.durationMs);
    putText(object, u"isrc"_s, release.isrc);
    putText(object, u"upc"_s, release.upc);
    if (!release.covers.isEmpty()) {
        QJsonArray covers;
        for (const meta::CoverRef &cover : release.covers) {
            covers.append(
                QJsonObject{{u"url"_s, cover.url.toString()}, {u"width"_s, cover.width}, {u"height"_s, cover.height}});
        }
        object.insert(u"covers"_s, covers);
    }
    putText(object, u"spotifyId"_s, release.spotifyId);
    putText(object, u"spotifyUrl"_s, release.spotifyUrl.toString());
    putText(object, u"spotifyAlbumId"_s, release.spotifyAlbumId);
    putText(object, u"deezerId"_s, release.deezerId);
    putText(object, u"deezerUrl"_s, release.deezerUrl.toString());
    putText(object, u"deezerAlbumId"_s, release.deezerAlbumId);
    putText(object, u"musicBrainzRecordingId"_s, release.musicBrainzRecordingId);
    putText(object, u"musicBrainzReleaseId"_s, release.musicBrainzReleaseId);
    putText(object, u"musicBrainzReleaseGroupId"_s, release.musicBrainzReleaseGroupId);
    putText(object, u"label"_s, release.label);
    putText(object, u"catalogNumber"_s, release.catalogNumber);
    putList(object, u"genres"_s, release.genres);
    if (release.labelSource) {
        object.insert(u"labelSource"_s, sourceKey(*release.labelSource));
    }
    if (release.genreSource) {
        object.insert(u"genreSource"_s, sourceKey(*release.genreSource));
    }
    putText(object, u"copyrightP"_s, release.copyrightP);
    return object;
}

meta::Release releaseFromJson(const QJsonObject &object)
{
    meta::Release release;
    release.source = sourceFrom(object.value(u"source"_s).toString()).value_or(meta::Source::Spotify);
    release.title = object.value(u"title"_s).toString();
    release.artists = list(object, u"artists"_s);
    release.album = object.value(u"album"_s).toString();
    release.albumArtists = list(object, u"albumArtists"_s);
    release.albumType = object.value(u"albumType"_s).toString();
    release.releaseDate = object.value(u"releaseDate"_s).toString();
    release.releaseDatePrecision = precisionFrom(object.value(u"releaseDatePrecision"_s).toString());
    release.trackNumber = optionalInt(object, u"trackNumber"_s);
    release.trackTotal = optionalInt(object, u"trackTotal"_s);
    release.discNumber = optionalInt(object, u"discNumber"_s);
    release.discTotal = optionalInt(object, u"discTotal"_s);
    release.durationMs = optionalInt64(object, u"durationMs"_s);
    release.isrc = object.value(u"isrc"_s).toString();
    release.upc = object.value(u"upc"_s).toString();
    const QJsonArray covers = object.value(u"covers"_s).toArray();
    for (const auto &value : covers) {
        const QJsonObject cover = value.toObject();
        const QUrl url(cover.value(u"url"_s).toString());
        if (url.isValid() && !url.isEmpty()) {
            release.covers.append(meta::CoverRef{
                .url = url, .width = cover.value(u"width"_s).toInt(), .height = cover.value(u"height"_s).toInt()});
        }
    }
    release.spotifyId = object.value(u"spotifyId"_s).toString();
    release.spotifyUrl = QUrl(object.value(u"spotifyUrl"_s).toString());
    release.spotifyAlbumId = object.value(u"spotifyAlbumId"_s).toString();
    release.deezerId = object.value(u"deezerId"_s).toString();
    release.deezerUrl = QUrl(object.value(u"deezerUrl"_s).toString());
    release.deezerAlbumId = object.value(u"deezerAlbumId"_s).toString();
    release.musicBrainzRecordingId = object.value(u"musicBrainzRecordingId"_s).toString();
    release.musicBrainzReleaseId = object.value(u"musicBrainzReleaseId"_s).toString();
    release.musicBrainzReleaseGroupId = object.value(u"musicBrainzReleaseGroupId"_s).toString();
    release.label = object.value(u"label"_s).toString();
    release.catalogNumber = object.value(u"catalogNumber"_s).toString();
    release.genres = list(object, u"genres"_s);
    release.labelSource = sourceFrom(object.value(u"labelSource"_s).toString());
    release.genreSource = sourceFrom(object.value(u"genreSource"_s).toString());
    release.copyrightP = object.value(u"copyrightP"_s).toString();
    return release;
}

QByteArray releaseToText(const meta::Release &release)
{
    return QJsonDocument(releaseToJson(release)).toJson(QJsonDocument::Compact);
}

std::optional<meta::Release> releaseFromText(const QByteArray &text)
{
    if (text.isEmpty()) {
        return std::nullopt;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(text, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return releaseFromJson(document.object());
}

} // namespace selecta::core
