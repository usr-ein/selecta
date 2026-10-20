// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/SpotifyJson.h"

#include "meta/detail/Identifiers.h"
#include "meta/support/Fixtures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
namespace sj = selecta::meta::detail::spotify;
using selecta::meta::detail::isValidMbid;
using selecta::meta::detail::normalizedBarcode;
using selecta::meta::detail::normalizedIsrc;
using selecta::meta::detail::releaseDate;
using selecta::meta::testing::fixture;

namespace {

QJsonObject load(const QString &name)
{
    return QJsonDocument::fromJson(fixture(u"spotify/"_s + name)).object();
}

} // namespace

TEST(Identifiers, IsrcIsNormalisedOrRejected)
{
    EXPECT_EQ(normalizedIsrc(u"GBCFB1700229"_s), u"GBCFB1700229"_s);
    EXPECT_EQ(normalizedIsrc(u"gb-cfb-17-00229"_s), u"GBCFB1700229"_s);
    EXPECT_EQ(normalizedIsrc(u"GB CFB 17 00229"_s), u"GBCFB1700229"_s);
    EXPECT_TRUE(normalizedIsrc(u"GBCFB170022"_s).isEmpty());  // 11 characters
    EXPECT_TRUE(normalizedIsrc(u"G1CFB1700229"_s).isEmpty()); // country must be letters
    EXPECT_TRUE(normalizedIsrc(u"GBCFB17002X9"_s).isEmpty()); // designation must be digits
    EXPECT_TRUE(normalizedIsrc(u""_s).isEmpty());
    EXPECT_TRUE(normalizedIsrc(QString(100, u'A')).isEmpty());
}

TEST(Identifiers, MbidsAreLowerCaseUuids)
{
    EXPECT_TRUE(isValidMbid(u"5b11f4ce-a62d-471e-81fc-a69a8278c7da"_s));
    EXPECT_FALSE(isValidMbid(u"5B11F4CE-A62D-471E-81FC-A69A8278C7DA"_s));
    EXPECT_FALSE(isValidMbid(u"5b11f4ce-a62d-471e-81fc"_s));
    EXPECT_FALSE(isValidMbid(u"../../etc/passwd"_s));
}

TEST(Identifiers, Barcodes)
{
    EXPECT_EQ(normalizedBarcode(u"5056000000017"_s), u"5056000000017"_s);
    EXPECT_EQ(normalizedBarcode(u" 00602445790074 "_s), u"00602445790074"_s);
    EXPECT_TRUE(normalizedBarcode(u"1234567"_s).isEmpty());
    EXPECT_TRUE(normalizedBarcode(u"123456789012345"_s).isEmpty());
    EXPECT_TRUE(normalizedBarcode(u"50560000000x7"_s).isEmpty());
}

TEST(Identifiers, ReleaseDatesKeepTheirPrecision)
{
    using P = DatePrecision;
    EXPECT_EQ(releaseDate(u"2017-06-09"_s, u"day"_s), std::make_pair(u"2017-06-09"_s, P::Day));
    EXPECT_EQ(releaseDate(u"2017-06"_s, u"month"_s), std::make_pair(u"2017-06"_s, P::Month));
    EXPECT_EQ(releaseDate(u"2017"_s, u"year"_s), std::make_pair(u"2017"_s, P::Year));
    EXPECT_EQ(releaseDate(u"2017-06-09"_s), std::make_pair(u"2017-06-09"_s, P::Day));
    EXPECT_EQ(releaseDate(u"2017-01-01"_s, u"year"_s), std::make_pair(u"2017"_s, P::Year)); // the hint coarsens
    EXPECT_EQ(releaseDate(u"2017-06-09T00:00:00Z"_s), std::make_pair(u"2017-06-09"_s, P::Day));
    EXPECT_EQ(releaseDate(u"2017-02-30"_s), std::make_pair(u"2017-02"_s, P::Month)); // impossible day dropped
    EXPECT_EQ(releaseDate(u"2017-13"_s), std::make_pair(u"2017"_s, P::Year));
    EXPECT_EQ(releaseDate(u"0000"_s, u"year"_s).second, P::Unknown);
    EXPECT_EQ(releaseDate(u""_s).second, P::Unknown);
    EXPECT_EQ(releaseDate(u"soon"_s).second, P::Unknown);
}

TEST(SpotifyJson, FullTrackMapsEveryField)
{
    const Release r = sj::track(load(u"track.json"_s));
    EXPECT_EQ(r.source, Source::Spotify);
    EXPECT_EQ(r.title, u"Glass Harbour - Extended Mix"_s);
    EXPECT_EQ(r.artists, (QStringList{u"Kestrel Moon"_s, u"Night Tram"_s}));
    EXPECT_EQ(r.artist(), u"Kestrel Moon, Night Tram"_s);
    EXPECT_EQ(r.album, u"Glass Harbour"_s);
    EXPECT_EQ(r.albumArtists, QStringList{u"Kestrel Moon"_s});
    EXPECT_EQ(r.albumType, u"single"_s);
    EXPECT_EQ(r.releaseDate, u"2026-03-13"_s);
    EXPECT_EQ(r.releaseDatePrecision, DatePrecision::Day);
    EXPECT_EQ(r.trackNumber, 2);
    EXPECT_EQ(r.trackTotal, 3);
    EXPECT_EQ(r.discNumber, 1);
    EXPECT_FALSE(r.discTotal.has_value()); // unknown without the whole album
    EXPECT_EQ(r.durationMs, 391000);
    EXPECT_EQ(r.isrc, u"GBXYZ2600101"_s); // upper-cased
    EXPECT_EQ(r.spotifyId, u"u8jzPde0IgxLd6GncfBAep"_s);
    EXPECT_EQ(r.spotifyUrl, QUrl(u"https://open.spotify.com/track/u8jzPde0IgxLd6GncfBAep"_s));
    EXPECT_EQ(r.spotifyAlbumId, u"KdNnFRIBXuDL7DxtpYlSXp"_s);
    // Covers: largest first, the non-HTTPS one dropped.
    ASSERT_EQ(r.covers.size(), 3);
    EXPECT_EQ(r.covers.at(0).width, 640);
    EXPECT_EQ(r.covers.at(0).url, QUrl(u"https://i.scdn.co/image/test-glass-harbour-640"_s));
    EXPECT_EQ(r.covers.at(2).width, 64);
    // Not from Spotify (D30): label and genre stay empty.
    EXPECT_TRUE(r.label.isEmpty());
    EXPECT_TRUE(r.genres.isEmpty());
    EXPECT_FALSE(r.labelSource.has_value());
}

TEST(SpotifyJson, MissingDataStaysMissing)
{
    const Release r = sj::track(load(u"track_minimal.json"_s));
    EXPECT_EQ(r.title, u"Side B"_s);
    EXPECT_EQ(r.artists, QStringList{u"Unknown Pressing"_s}); // the nameless artist is dropped
    EXPECT_FALSE(r.durationMs.has_value());                   // 0 isn't a duration
    EXPECT_FALSE(r.trackNumber.has_value());                  // 0 isn't a track number
    EXPECT_FALSE(r.discNumber.has_value());
    EXPECT_FALSE(r.trackTotal.has_value());
    EXPECT_TRUE(r.isrc.isEmpty()); // malformed ISRC dropped, not "fixed"
    EXPECT_TRUE(r.releaseDate.isEmpty());
    EXPECT_EQ(r.releaseDatePrecision, DatePrecision::Unknown);
    EXPECT_TRUE(r.covers.isEmpty());
    EXPECT_TRUE(r.albumArtists.isEmpty());
    EXPECT_EQ(r.albumType, u"compilation"_s);
    EXPECT_EQ(r.spotifyUrl, QUrl(u"https://open.spotify.com/track/tjfIZ4SOcMz9CPVNPkNa1H"_s)); // built from the ID
}

TEST(SpotifyJson, GarbageTypesAreIgnored)
{
    const QJsonObject o = QJsonDocument::fromJson(
                              R"({"name": 5, "artists": "x", "duration_ms": "391000", "track_number": 2.5,
                                  "external_ids": [], "album": "nope", "id": 12})")
                              .object();
    const Release r = sj::track(o);
    EXPECT_TRUE(r.title.isEmpty());
    EXPECT_TRUE(r.artists.isEmpty());
    EXPECT_FALSE(r.durationMs.has_value());
    EXPECT_FALSE(r.trackNumber.has_value());
    EXPECT_TRUE(r.spotifyId.isEmpty());
    EXPECT_TRUE(r.album.isEmpty());
}

TEST(SpotifyJson, AlbumWithItsTracks)
{
    Album tidal = sj::album(load(u"album.json"_s));
    EXPECT_EQ(tidal.id, u"8rESQedUStPKR0CsTy4Qwb"_s);
    EXPECT_EQ(tidal.name, u"Tidal Lines"_s);
    EXPECT_EQ(tidal.totalTracks, 4);
    EXPECT_EQ(tidal.upc, u"5056000000017"_s);
    EXPECT_EQ(tidal.copyrightP, u"℗ 2025 Lantern Grove Records"_s);
    EXPECT_EQ(tidal.releaseDate, u"2025-11"_s);
    EXPECT_EQ(tidal.releaseDatePrecision, DatePrecision::Month);
    ASSERT_EQ(tidal.tracks.size(), 2);
    sj::appendAlbumTracks(tidal, load(u"album_tracks_page2.json"_s).value("items"_L1).toArray());
    sj::finishAlbum(tidal);
    ASSERT_EQ(tidal.tracks.size(), 4);
    EXPECT_EQ(tidal.discTotal, 2);
    for (const Release &albumTrack : std::as_const(tidal.tracks)) {
        EXPECT_EQ(albumTrack.album, u"Tidal Lines"_s);
        EXPECT_EQ(albumTrack.albumArtists, QStringList{u"Kestrel Moon"_s});
        EXPECT_EQ(albumTrack.trackTotal, 4);
        EXPECT_EQ(albumTrack.discTotal, 2);
        EXPECT_EQ(albumTrack.upc, u"5056000000017"_s);
        EXPECT_EQ(albumTrack.spotifyAlbumId, u"8rESQedUStPKR0CsTy4Qwb"_s);
        EXPECT_EQ(albumTrack.covers.size(), 2);
        EXPECT_TRUE(albumTrack.isrc.isEmpty()); // album tracks are simplified objects
        EXPECT_FALSE(albumTrack.spotifyId.isEmpty());
    }
    EXPECT_EQ(tidal.tracks.at(2).discNumber, 2);
    EXPECT_EQ(tidal.tracks.at(2).trackNumber, 1);
}

TEST(SpotifyJson, PlaylistSummaries)
{
    const QJsonArray items = load(u"playlists_page1.json"_s).value("items"_L1).toArray();
    const PlaylistSummary own = sj::playlist(items.at(0).toObject());
    EXPECT_EQ(own.id, u"OzdmenCkhvMdgaKjIg8xNb"_s);
    EXPECT_EQ(own.name, u"Warm-up"_s);
    EXPECT_EQ(own.owner, u"Test Listener"_s);
    EXPECT_EQ(own.trackCount, 4); // from "items"
    EXPECT_FALSE(own.collaborative);
    const PlaylistSummary shared = sj::playlist(items.at(1).toObject());
    EXPECT_EQ(shared.owner, u"anotherlistener"_s); // no display name: the ID
    EXPECT_EQ(shared.trackCount, 120);             // from the older "tracks"
    EXPECT_TRUE(shared.collaborative);
}

TEST(SpotifyJson, PlaylistItemsSkipEpisodesAndKeepLocalFilesAsText)
{
    const QJsonArray items = load(u"playlist_items.json"_s).value("items"_L1).toArray();
    const std::optional<Release> track = sj::itemTrack(items.at(0).toObject());
    ASSERT_TRUE(track.has_value());
    EXPECT_EQ(track->spotifyId, u"u8jzPde0IgxLd6GncfBAep"_s);
    const std::optional<Release> local = sj::itemTrack(items.at(1).toObject());
    ASSERT_TRUE(local.has_value());
    EXPECT_TRUE(local->spotifyId.isEmpty());
    EXPECT_TRUE(local->spotifyUrl.isEmpty());
    EXPECT_EQ(local->title, u"Unreleased VIP"_s);
    EXPECT_EQ(local->artists, QStringList{u"Dubplate Special"_s});
    EXPECT_EQ(local->durationMs, 333000);
    EXPECT_FALSE(sj::itemTrack(items.at(2).toObject()).has_value()); // episode
    EXPECT_FALSE(sj::itemTrack(items.at(3).toObject()).has_value()); // null track
}

TEST(SpotifyJson, OnlyDevelopmentModeEndpoints)
{
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/search"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/tracks/u8jzPde0IgxLd6GncfBAep"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/albums/8rESQedUStPKR0CsTy4Qwb"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/albums/8rESQedUStPKR0CsTy4Qwb/tracks"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/me/playlists"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/playlists/OzdmenCkhvMdgaKjIg8xNb"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/playlists/OzdmenCkhvMdgaKjIg8xNb/items"_s));
    EXPECT_TRUE(sj::isDevelopmentModeEndpoint(u"/me/tracks"_s));
    // Removed in 2024–2026 or never used.
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/tracks"_s)); // batch
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/albums"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/playlists/OzdmenCkhvMdgaKjIg8xNb/tracks"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/artists/fKtHF4vUCsMehGAkWvj7FA/top-tracks"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/recommendations"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/audio-features/u8jzPde0IgxLd6GncfBAep"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/browse/new-releases"_s));
    EXPECT_FALSE(sj::isDevelopmentModeEndpoint(u"/users/someone/playlists"_s));
}
