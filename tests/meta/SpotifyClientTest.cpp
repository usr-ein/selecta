// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyClient.h"

#include "meta/MetaCache.h"
#include "meta/SpotifyAuth.h"
#include "meta/TokenStore.h"
#include "meta/detail/ServiceGate.h"
#include "meta/detail/SpotifyJson.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Fixtures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
using selecta::base::ManualClock;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::fixture;
using selecta::meta::testing::header;
using selecta::meta::testing::spinUntil;

namespace {

const QString kClientId = u"0123456789abcdef0123456789abcdef"_s;
const QString kTokenUrl = u"^https://accounts\\.test/api/token$"_s;
const QString kTrackId = u"u8jzPde0IgxLd6GncfBAep"_s;
const QString kAlbumId = u"8rESQedUStPKR0CsTy4Qwb"_s;
const QString kPlaylistId = u"OzdmenCkhvMdgaKjIg8xNb"_s;

QByteArray tokenJson(const QByteArray &access, const QString &scope = QString())
{
    QJsonObject o;
    o.insert("access_token"_L1, QString::fromLatin1(access));
    o.insert("token_type"_L1, u"Bearer"_s);
    o.insert("expires_in"_L1, 3600);
    if (!scope.isEmpty()) {
        o.insert("scope"_L1, scope);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

spotify::PastedLinks paste(const QString &text)
{
    return spotify::parsePastedLinks(text);
}

QString api(const QString &pathRegex)
{
    return u"^https://api\\.test/v1"_s + pathRegex;
}

class SpotifyClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        SpotifyAuth::Config config;
        config.clientId = kClientId;
        config.accountsUrl = QUrl(u"https://accounts.test"_s);
        config.openUrl = [](const QUrl &) { return false; };
        auth = std::make_unique<SpotifyAuth>(config, &network, pacer, store, clock);
        client = std::make_unique<SpotifyClient>(*auth, &network, pacer, &cache, clock, nullptr,
                                                 QUrl(u"https://api.test/v1"_s));
    }

    void signInClientCredentials()
    {
        network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("CCTOKEN")));
        const auto result = selecta::meta::testing::await(auth->signInWithClientSecret("SECRET"));
        ASSERT_TRUE(result.has_value() && result->ok());
    }

    // A user sign-in restored from the store; the first API call refreshes it (answer queued here).
    void signInUser(const QStringList &scopes)
    {
        QJsonObject record;
        record.insert("v"_L1, 1);
        record.insert("client_id"_L1, kClientId);
        record.insert("refresh_token"_L1, u"REFRESH"_s);
        record.insert("authorised_at"_L1, QDateTime::currentDateTimeUtc().addDays(-1).toString(Qt::ISODate));
        record.insert("scopes"_L1, QJsonArray::fromStringList(scopes));
        ASSERT_TRUE(selecta::meta::testing::await(
                        store.write(u"spotify/"_s + kClientId, QJsonDocument(record).toJson(QJsonDocument::Compact)))
                        ->ok());
        ASSERT_TRUE(selecta::meta::testing::await(auth->restore())->ok());
        network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("USERTOKEN", scopes.join(u' '))));
    }

    template <typename T>
    std::optional<T> wait(const QFuture<T> &future)
    {
        return awaitAdvancing(future, clock, 1s, 8000);
    }

    QList<selecta::meta::testing::RecordedRequest> apiRequests() const
    {
        QList<selecta::meta::testing::RecordedRequest> list;
        for (const auto &request : network.requests()) {
            if (request.url.host() == u"api.test"_s) {
                list.append(request);
            }
        }
        return list;
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    InMemoryTokenStore store;
    std::unique_ptr<SpotifyAuth> auth;
    std::unique_ptr<SpotifyClient> client;
};

} // namespace

TEST_F(SpotifyClientTest, SearchQueryUsesFieldFiltersAndCleansValues)
{
    TrackQuery query;
    query.artist = u"Kestrel Moon"_s;
    query.title = u"Glass \"Harbour\"\n"_s;
    query.version = u"Extended Mix"_s;
    query.album = u"  Glass   Harbour "_s;
    EXPECT_EQ(SpotifyClient::searchQueryText(query),
              u"track:\"Glass Harbour\" artist:\"Kestrel Moon\" album:\"Glass Harbour\" Extended Mix"_s);
    TrackQuery titleOnly;
    titleOnly.title = u"Glass Harbour"_s;
    EXPECT_EQ(SpotifyClient::searchQueryText(titleOnly), u"track:\"Glass Harbour\""_s);
    TrackQuery longOne;
    longOne.title = QString(500, u'x');
    EXPECT_LE(SpotifyClient::searchQueryText(longOne).size(), 120);
}

TEST_F(SpotifyClientTest, SearchMapsEveryHit)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/search\\?"_s), FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    TrackQuery query;
    query.artist = u"Kestrel Moon"_s;
    query.title = u"Glass Harbour"_s;
    const auto result = wait(client->searchTracks(query));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    ASSERT_EQ(result->value().size(), 3);
    EXPECT_EQ(result->value().at(0).title, u"Glass Harbour - Extended Mix"_s);
    EXPECT_EQ(result->value().at(1).durationMs, 201500);
    EXPECT_EQ(result->value().at(2).artists, (QStringList{u"Kestrel Moon"_s, u"Night Tram"_s}));
    EXPECT_EQ(result->value().at(2).isrc, u"GBXYZ2600103"_s);

    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 1);
    const QUrlQuery sent(requests.first().url);
    EXPECT_EQ(sent.queryItemValue(u"q"_s, QUrl::FullyDecoded), u"track:\"Glass Harbour\" artist:\"Kestrel Moon\""_s);
    EXPECT_EQ(sent.queryItemValue(u"type"_s), u"track"_s);
    EXPECT_EQ(sent.queryItemValue(u"limit"_s), u"10"_s);
    EXPECT_EQ(requests.first().header("Authorization"), "Bearer CCTOKEN");
}

TEST_F(SpotifyClientTest, SearchLimitIsClampedToDevelopmentMode)
{
    signInClientCredentials();
    network.setDefault("GET", api(u"/search\\?"_s), FakeResponse::json(R"({"tracks":{"items":[],"total":0}})"));
    TrackQuery query;
    query.title = u"x y"_s;
    query.limit = 50;
    ASSERT_TRUE(wait(client->searchTracks(query))->ok());
    query.limit = 0;
    query.title = u"x y z"_s;
    ASSERT_TRUE(wait(client->searchTracks(query))->ok());
    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(QUrlQuery(requests.at(0).url).queryItemValue(u"limit"_s), u"10"_s);
    EXPECT_EQ(QUrlQuery(requests.at(1).url).queryItemValue(u"limit"_s), u"1"_s);
}

TEST_F(SpotifyClientTest, EmptySearchIsRefusedLocally)
{
    signInClientCredentials();
    TrackQuery query;
    query.title = u"  \"\"  "_s;
    const auto result = wait(client->searchTracks(query));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(apiRequests().isEmpty());
}

TEST_F(SpotifyClientTest, IsrcSearchNormalisesTheCode)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/search\\?"_s), FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    ASSERT_TRUE(wait(client->searchIsrc(u"gb-xyz-26-00101"_s))->ok());
    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(QUrlQuery(requests.first().url).queryItemValue(u"q"_s, QUrl::FullyDecoded), u"isrc:GBXYZ2600101"_s);
    EXPECT_EQ(wait(client->searchIsrc(u"nope"_s))->error().code, ErrorCode::InvalidArgument);
}

TEST_F(SpotifyClientTest, TrackLookupAndErrors)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId + u"$"_s),
                    FakeResponse::json(fixture(u"spotify/track.json"_s)));
    const auto track = wait(client->track(kTrackId));
    ASSERT_TRUE(track.has_value() && track->ok());
    EXPECT_EQ(track->value().spotifyId, kTrackId);
    EXPECT_EQ(track->value().isrc, u"GBXYZ2600101"_s);

    EXPECT_EQ(wait(client->track(u"short"_s))->error().code, ErrorCode::InvalidArgument);

    const QString missing = u"tjfIZ4SOcMz9CPVNPkNa1H"_s;
    network.enqueue("GET", api(u"/tracks/"_s + missing), FakeResponse::json(R"({"error":{"status":404}})", 404));
    EXPECT_EQ(wait(client->track(missing))->error().code, ErrorCode::NotFound);

    const QString broken = u"edcm4pMbXDuCL1mHoOsFaQ"_s;
    network.enqueue("GET", api(u"/tracks/"_s + broken), FakeResponse::json("{\"name\": "));
    EXPECT_EQ(wait(client->track(broken))->error().code, ErrorCode::BadResponse);

    const QString empty = u"fDPrAJ71fTquWoGsbeKXgz"_s;
    network.enqueue("GET", api(u"/tracks/"_s + empty), FakeResponse::json("{}"));
    EXPECT_EQ(wait(client->track(empty))->error().code, ErrorCode::BadResponse);
}

TEST_F(SpotifyClientTest, AlbumIsPagedAndDiscTotalsComputed)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"$"_s),
                    FakeResponse::json(fixture(u"spotify/album.json"_s)));
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"/tracks\\?"_s),
                    FakeResponse::json(fixture(u"spotify/album_tracks_page2.json"_s)));
    const auto album = wait(client->album(kAlbumId));
    ASSERT_TRUE(album.has_value());
    ASSERT_TRUE(album->ok()) << album->error().toString().toStdString();
    EXPECT_EQ(album->value().tracks.size(), 4);
    EXPECT_EQ(album->value().discTotal, 2);
    EXPECT_EQ(album->value().tracks.last().discTotal, 2);
    EXPECT_EQ(album->value().tracks.last().title, u"Undertow - Dub"_s);
    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(QUrlQuery(requests.at(1).url).queryItemValue(u"offset"_s), u"2"_s);
    EXPECT_EQ(QUrlQuery(requests.at(1).url).queryItemValue(u"limit"_s), u"50"_s);
}

TEST_F(SpotifyClientTest, AlbumFailsWhenAPageFails)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"$"_s),
                    FakeResponse::json(fixture(u"spotify/album.json"_s)));
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"/tracks\\?"_s), FakeResponse::json("{}", 404));
    EXPECT_EQ(wait(client->album(kAlbumId))->error().code, ErrorCode::NotFound);
}

TEST_F(SpotifyClientTest, CatalogueAnswersAreCachedAndDroppedOnSignOut)
{
    signInClientCredentials();
    network.setDefault("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    ASSERT_TRUE(wait(client->track(kTrackId))->ok());
    ASSERT_TRUE(wait(client->track(kTrackId))->ok());
    EXPECT_EQ(apiRequests().size(), 1);
    ASSERT_TRUE(selecta::meta::testing::await(auth->signOut())->ok());
    EXPECT_EQ(cache.count(), 0);
    EXPECT_EQ(wait(client->track(kTrackId))->error().code, ErrorCode::SignInRequired);
}

TEST_F(SpotifyClientTest, NotSignedInFailsWithoutARequest)
{
    const auto result = wait(client->track(kTrackId));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::SignInRequired);
    EXPECT_EQ(network.requestCount(), 0);
    EXPECT_FALSE(client->isAvailable());
}

TEST_F(SpotifyClientTest, UnauthorizedRefreshesOnceThenRetries)
{
    signInClientCredentials();
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(R"({"error":{"status":401}})", 401));
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("CCTOKEN2")));
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    const auto track = wait(client->track(kTrackId));
    ASSERT_TRUE(track.has_value());
    ASSERT_TRUE(track->ok()) << track->error().toString().toStdString();
    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests.at(0).header("Authorization"), "Bearer CCTOKEN");
    EXPECT_EQ(requests.at(1).header("Authorization"), "Bearer CCTOKEN2");
}

TEST_F(SpotifyClientTest, SecondUnauthorizedGivesUp)
{
    signInClientCredentials();
    network.setDefault("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json("{}", 401));
    network.enqueue("POST", kTokenUrl, FakeResponse::json(tokenJson("CCTOKEN2")));
    const auto track = wait(client->track(kTrackId));
    ASSERT_TRUE(track.has_value());
    EXPECT_EQ(track->error().code, ErrorCode::SignInRequired);
    EXPECT_EQ(apiRequests().size(), 2);
}

TEST_F(SpotifyClientTest, ShortRateLimitIsWaitedOut)
{
    signInClientCredentials();
    FakeResponse limited = FakeResponse::json(R"({"error":{"status":429,"message":"API rate limit exceeded"}})", 429);
    limited.headers.append(header("Retry-After", "3"));
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), limited);
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    const auto track = wait(client->track(kTrackId));
    ASSERT_TRUE(track.has_value());
    EXPECT_TRUE(track->ok());
    EXPECT_EQ(apiRequests().size(), 2);
}

TEST_F(SpotifyClientTest, QuotaExceededStopsSpotifyCalls)
{
    signInClientCredentials();
    FakeResponse quota =
        FakeResponse::json(R"({"error":{"status":429,"message":"Quota exceeded","reason":"QUOTA_EXCEEDED"}})", 429);
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), quota);
    const auto first = wait(client->track(kTrackId));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(first->error().retryAfter, 3600s);
    // Further calls fail at once, without a request, until the hour is over.
    TrackQuery query;
    query.title = u"Glass Harbour"_s;
    const auto second = selecta::meta::testing::await(client->searchTracks(query));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->error().code, ErrorCode::RateLimited);
    EXPECT_GT(second->error().retryAfter.value_or(0s), 3000s);
    EXPECT_EQ(apiRequests().size(), 1);
}

TEST_F(SpotifyClientTest, QuotaRetryAfterHeaderIsHonoured)
{
    signInClientCredentials();
    FakeResponse quota = FakeResponse::json(R"({"error":{"status":429,"reason":"QUOTA_EXCEEDED"}})", 429);
    quota.headers.append(header("Retry-After", "120"));
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), quota);
    EXPECT_EQ(wait(client->track(kTrackId))->error().retryAfter, 120s);
}

TEST_F(SpotifyClientTest, PlaylistsArePagedAndNeedTheirScopes)
{
    signInUser(SpotifyAuth::playlistScopes());
    network.enqueue("GET", api(u"/me/playlists\\?"_s), FakeResponse::json(fixture(u"spotify/playlists_page1.json"_s)));
    network.enqueue("GET", api(u"/me/playlists\\?"_s), FakeResponse::json(fixture(u"spotify/playlists_page2.json"_s)));
    const auto playlists = wait(client->myPlaylists());
    ASSERT_TRUE(playlists.has_value());
    ASSERT_TRUE(playlists->ok()) << playlists->error().toString().toStdString();
    ASSERT_EQ(playlists->value().size(), 3); // the entry with a broken ID is dropped
    EXPECT_EQ(playlists->value().at(2).name, u"Closing Set"_s);
    const auto requests = apiRequests();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(QUrlQuery(requests.at(1).url).queryItemValue(u"offset"_s), u"2"_s);
    EXPECT_EQ(requests.at(0).header("Authorization"), "Bearer USERTOKEN");
}

TEST_F(SpotifyClientTest, MissingScopesFailWithoutARequest)
{
    signInUser({u"user-library-read"_s});
    EXPECT_EQ(wait(client->myPlaylists())->error().code, ErrorCode::MissingScope);
    EXPECT_EQ(wait(client->playlistTracks(kPlaylistId))->error().code, ErrorCode::MissingScope);
    EXPECT_TRUE(apiRequests().isEmpty());
}

TEST_F(SpotifyClientTest, ClientCredentialsCantReadUserData)
{
    signInClientCredentials();
    EXPECT_EQ(wait(client->likedSongs())->error().code, ErrorCode::MissingScope);
    EXPECT_TRUE(apiRequests().isEmpty());
}

TEST_F(SpotifyClientTest, PlaylistTracksKeepLocalFilesAndSkipEpisodes)
{
    signInUser(SpotifyAuth::playlistScopes());
    network.enqueue("GET", api(u"/playlists/"_s + kPlaylistId + u"\\?fields=name$"_s),
                    FakeResponse::json(R"({"name":"Warm-up"})"));
    network.enqueue("GET", api(u"/playlists/"_s + kPlaylistId + u"/items\\?"_s),
                    FakeResponse::json(fixture(u"spotify/playlist_items.json"_s)));
    const auto list = wait(client->playlistTracks(kPlaylistId));
    ASSERT_TRUE(list.has_value());
    ASSERT_TRUE(list->ok()) << list->error().toString().toStdString();
    EXPECT_EQ(list->value().name, u"Warm-up"_s);
    ASSERT_EQ(list->value().tracks.size(), 2);
    EXPECT_EQ(list->value().tracks.at(0).spotifyId, kTrackId);
    EXPECT_TRUE(list->value().tracks.at(1).spotifyId.isEmpty());
    EXPECT_EQ(list->value().skipped, 2);
    EXPECT_FALSE(list->value().truncated);
}

TEST_F(SpotifyClientTest, SomeoneElsesPlaylistIsForbiddenWithAHint)
{
    signInUser(SpotifyAuth::playlistScopes());
    network.enqueue("GET", api(u"/playlists/"_s + kPlaylistId + u"\\?"_s), FakeResponse::json(R"({"name":"Hits"})"));
    network.enqueue("GET", api(u"/playlists/"_s + kPlaylistId + u"/items\\?"_s),
                    FakeResponse::json(R"({"error":{"status":403,"message":"Forbidden"}})", 403));
    const auto list = wait(client->playlistTracks(kPlaylistId));
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->error().code, ErrorCode::Forbidden);
    EXPECT_TRUE(list->error().message.contains(u"paste"_s));
}

TEST_F(SpotifyClientTest, LikedSongs)
{
    signInUser(SpotifyAuth::libraryScopes());
    network.enqueue("GET", api(u"/me/tracks\\?"_s), FakeResponse::json(fixture(u"spotify/liked_songs.json"_s)));
    const auto list = wait(client->likedSongs());
    ASSERT_TRUE(list.has_value());
    ASSERT_TRUE(list->ok()) << list->error().toString().toStdString();
    EXPECT_EQ(list->value().name, u"Liked Songs"_s);
    ASSERT_EQ(list->value().tracks.size(), 1);
    EXPECT_EQ(list->value().tracks.first().isrc, u"GBXYZ2500202"_s);
}

TEST_F(SpotifyClientTest, PastedLinksAreLookedUpInOrder)
{
    signInClientCredentials();
    const QString second = u"fJBd0Kh8oOOL8dKLzdocJ2"_s;
    const QString gone = u"tjfIZ4SOcMz9CPVNPkNa1H"_s;
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    network.enqueue("GET", api(u"/tracks/"_s + gone), FakeResponse::json("{}", 404));
    QJsonObject other = QJsonDocument::fromJson(fixture(u"spotify/track.json"_s)).object();
    other.insert("id"_L1, second);
    other.insert("name"_L1, u"Glass Harbour - Radio Edit"_s);
    network.enqueue("GET", api(u"/tracks/"_s + second), FakeResponse::json(QJsonDocument(other).toJson()));
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"$"_s),
                    FakeResponse::json(fixture(u"spotify/album.json"_s)));
    network.enqueue("GET", api(u"/albums/"_s + kAlbumId + u"/tracks\\?"_s),
                    FakeResponse::json(fixture(u"spotify/album_tracks_page2.json"_s)));
    const spotify::PastedLinks links =
        paste(u"https://open.spotify.com/track/"_s + kTrackId + u"\nspotify:track:"_s + gone + u"\nspotify:track:"_s +
              second + u"\nhttps://open.spotify.com/album/"_s + kAlbumId);
    const auto imported = wait(client->importLinks(links));
    ASSERT_TRUE(imported.has_value());
    ASSERT_TRUE(imported->ok()) << imported->error().toString().toStdString();
    ASSERT_EQ(imported->value().tracks.size(), 6);
    EXPECT_EQ(imported->value().tracks.at(0).spotifyId, kTrackId);
    EXPECT_EQ(imported->value().tracks.at(1).spotifyId, second);
    EXPECT_EQ(imported->value().tracks.at(2).title, u"Low Water"_s);
    ASSERT_EQ(imported->value().failed.size(), 1);
    EXPECT_EQ(imported->value().failed.first().first.id, gone);
    EXPECT_EQ(imported->value().failed.first().second.code, ErrorCode::NotFound);
}

TEST_F(SpotifyClientTest, PastedLinksStopAtAQuotaAndKeepWhatWasFound)
{
    signInClientCredentials();
    const QString second = u"fJBd0Kh8oOOL8dKLzdocJ2"_s;
    const QString third = u"isAjIhKtJ0RlgLKOmxgJTe"_s;
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    network.enqueue("GET", api(u"/tracks/"_s + second),
                    FakeResponse::json(R"({"error":{"status":429,"reason":"QUOTA_EXCEEDED"}})", 429));
    const spotify::PastedLinks links =
        paste(u"spotify:track:"_s + kTrackId + u"\nspotify:track:"_s + second + u"\nspotify:track:"_s + third);
    const auto imported = wait(client->importLinks(links));
    ASSERT_TRUE(imported.has_value());
    ASSERT_TRUE(imported->ok());
    EXPECT_EQ(imported->value().tracks.size(), 1);
    ASSERT_EQ(imported->value().failed.size(), 2); // the quota one and the one never tried
    EXPECT_EQ(imported->value().failed.at(1).second.code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(apiRequests().size(), 2);
}

TEST_F(SpotifyClientTest, PastedLinksWithNothingFoundFailWithTheStopError)
{
    const spotify::PastedLinks links = paste(u"spotify:track:"_s + kTrackId);
    const auto imported = wait(client->importLinks(links));
    ASSERT_TRUE(imported.has_value());
    EXPECT_EQ(imported->error().code, ErrorCode::SignInRequired);
    EXPECT_TRUE(wait(client->importLinks({}))->ok()); // nothing to do
}

TEST_F(SpotifyClientTest, CancellingAbortsTheRequest)
{
    signInClientCredentials();
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), hang);
    QFuture<Result<Release>> future = client->track(kTrackId);
    ASSERT_TRUE(spinUntil([&] { return apiRequests().size() == 1; }));
    future.cancel();
    ASSERT_TRUE(spinUntil([&] { return network.liveReplies() == 0; }));
    EXPECT_TRUE(future.isCanceled());
}

TEST_F(SpotifyClientTest, DestroyingTheClientEndsPendingImports)
{
    signInClientCredentials();
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", api(u"/tracks/"_s + kTrackId), hang);
    QFuture<Result<LinkImport>> future = client->importLinks(paste(u"spotify:track:"_s + kTrackId));
    ASSERT_TRUE(spinUntil([&] { return apiRequests().size() == 1; }));
    client.reset();
    ASSERT_TRUE(spinUntil([&] { return future.isFinished(); }));
    EXPECT_TRUE(future.isCanceled());
}

TEST_F(SpotifyClientTest, EveryRequestIsADevelopmentModeEndpointWithAPermit)
{
    signInUser(SpotifyAuth::playlistScopes() + SpotifyAuth::libraryScopes());
    network.setDefault("GET", api(u"/search"_s), FakeResponse::json(fixture(u"spotify/search_tracks.json"_s)));
    network.setDefault("GET", api(u"/tracks/"_s), FakeResponse::json(fixture(u"spotify/track.json"_s)));
    network.setDefault("GET", api(u"/albums/"_s + kAlbumId + u"$"_s),
                       FakeResponse::json(fixture(u"spotify/album.json"_s)));
    network.setDefault("GET", api(u"/albums/"_s + kAlbumId + u"/tracks"_s),
                       FakeResponse::json(fixture(u"spotify/album_tracks_page2.json"_s)));
    network.setDefault("GET", api(u"/me/playlists"_s), FakeResponse::json(fixture(u"spotify/playlists_page2.json"_s)));
    network.setDefault("GET", api(u"/playlists/"_s + kPlaylistId + u"\\?"_s), FakeResponse::json(R"({"name":"x"})"));
    network.setDefault("GET", api(u"/playlists/"_s + kPlaylistId + u"/items"_s),
                       FakeResponse::json(fixture(u"spotify/playlist_items.json"_s)));
    network.setDefault("GET", api(u"/me/tracks"_s), FakeResponse::json(fixture(u"spotify/liked_songs.json"_s)));
    TrackQuery query;
    query.title = u"Glass Harbour"_s;
    ASSERT_TRUE(wait(client->searchTracks(query))->ok());
    ASSERT_TRUE(wait(client->searchIsrc(u"GBXYZ2600101"_s))->ok());
    ASSERT_TRUE(wait(client->track(kTrackId))->ok());
    ASSERT_TRUE(wait(client->album(kAlbumId))->ok());
    ASSERT_TRUE(wait(client->myPlaylists())->ok());
    ASSERT_TRUE(wait(client->playlistTracks(kPlaylistId))->ok());
    ASSERT_TRUE(wait(client->likedSongs())->ok());
    const auto requests = apiRequests();
    ASSERT_GE(requests.size(), 8);
    for (const auto &request : requests) {
        const QString path = request.url.path().mid(QStringLiteral("/v1").size());
        EXPECT_TRUE(detail::spotify::isDevelopmentModeEndpoint(path)) << path.toStdString();
        EXPECT_TRUE(request.header("Authorization").startsWith("Bearer "));
    }
    int spotifyPermits = 0;
    for (const auto operation : pacer.acquired()) {
        spotifyPermits += operation == selecta::governor::Operation::SpotifyRequest ? 1 : 0;
    }
    EXPECT_EQ(spotifyPermits, network.requestCount()); // token requests included: one permit each
    for (const auto &record : pacer.records()) {
        EXPECT_FALSE(record.target.contains(u"USERTOKEN"_s));
        EXPECT_FALSE(record.target.contains(u"REFRESH"_s));
    }
}
