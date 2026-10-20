// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/DeezerClient.h"
#include "meta/MetaCache.h"
#include "meta/detail/DeezerJson.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Fixtures.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
namespace dz = selecta::meta::detail::deezer;
using selecta::base::ManualClock;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::fixture;

namespace {

QJsonObject load(const QString &name)
{
    return QJsonDocument::fromJson(fixture(u"deezer/"_s + name)).object();
}

QString dzUrl(const QString &pathRegex)
{
    return u"^https://dz\\.test"_s + pathRegex;
}

class DeezerClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        client = std::make_unique<DeezerClient>(&network, pacer, &cache, clock, nullptr, QUrl(u"https://dz.test"_s));
    }

    template <typename T>
    std::optional<T> wait(const QFuture<T> &future)
    {
        return awaitAdvancing(future, clock, 1s, 8000);
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    std::unique_ptr<DeezerClient> client;
};

} // namespace

TEST(DeezerJson, SearchHitsAreMappedAndBrokenOnesDropped)
{
    const QList<DeezerTrack> hits = dz::searchHits(load(u"search_glass_harbour.json"_s));
    ASSERT_EQ(hits.size(), 4); // no id and the episode are dropped
    const DeezerTrack &extended = hits.at(0);
    EXPECT_EQ(extended.release.source, Source::Deezer);
    EXPECT_EQ(extended.release.title, u"Glass Harbour (Extended Mix)"_s);
    EXPECT_EQ(extended.titleVersion, u"(Extended Mix)"_s);
    EXPECT_EQ(extended.release.artists, QStringList{u"Kestrel Moon"_s});
    EXPECT_EQ(extended.release.durationMs, 391000);
    EXPECT_EQ(extended.release.isrc, u"GBXYZ2600101"_s);
    EXPECT_EQ(extended.release.deezerId, u"3135553"_s);
    EXPECT_EQ(extended.release.deezerUrl, QUrl(u"https://www.deezer.com/track/3135553"_s));
    EXPECT_EQ(extended.release.album, u"Glass Harbour"_s);
    EXPECT_EQ(extended.release.deezerAlbumId, u"302127"_s);
    ASSERT_EQ(extended.release.covers.size(), 4);
    EXPECT_EQ(extended.release.covers.first().width, 1000);
    EXPECT_TRUE(extended.previewUrl.toString().startsWith(u"https://cdnt-preview.dzcdn.net/"_s));
    EXPECT_TRUE(hits.at(3).previewUrl.isEmpty()); // not HTTPS on Deezer's CDN: dropped
    EXPECT_FALSE(extended.bpm.has_value());
}

TEST(DeezerJson, FullTrackHasContributorsPositionsAndNoZeroBpm)
{
    const std::optional<DeezerTrack> track = dz::track(load(u"track_isrc.json"_s));
    ASSERT_TRUE(track.has_value());
    EXPECT_EQ(track->release.artists, (QStringList{u"Kestrel Moon"_s, u"Clara Vale"_s}));
    EXPECT_EQ(track->release.trackNumber, 2);
    EXPECT_EQ(track->release.discNumber, 1);
    EXPECT_EQ(track->release.releaseDate, u"2026-03-13"_s);
    EXPECT_EQ(track->release.releaseDatePrecision, DatePrecision::Day);
    EXPECT_FALSE(track->bpm.has_value()); // 0 means "not analysed"
    QJsonObject withBpm = load(u"track_isrc.json"_s);
    withBpm.insert("bpm"_L1, 126.05);
    EXPECT_DOUBLE_EQ(dz::track(withBpm)->bpm.value_or(0), 126.05);
}

TEST(DeezerJson, AlbumHasLabelGenresAndBarcode)
{
    const std::optional<DeezerAlbum> album = dz::album(load(u"album.json"_s));
    ASSERT_TRUE(album.has_value());
    EXPECT_EQ(album->label, u"Lantern Grove Records"_s);
    EXPECT_EQ(album->upc, u"5056000000024"_s);
    EXPECT_EQ(album->genres, (QStringList{u"Electro"_s, u"Dance"_s}));
    EXPECT_EQ(album->recordType, u"single"_s);
    EXPECT_EQ(album->trackCount, 3);
    EXPECT_EQ(album->covers.first().width, 1000);
}

TEST(DeezerJson, ErrorCodes)
{
    EXPECT_EQ(dz::errorCode(QJsonDocument::fromJson(R"({"error":{"code":800,"message":"no data"}})").object()), 800);
    EXPECT_EQ(dz::errorCode(QJsonDocument::fromJson(R"({"error":{"message":"?"}})").object()), -1);
    EXPECT_FALSE(dz::errorCode(load(u"album.json"_s)).has_value());
}

TEST(DeezerJson, PreviewExpiryFromTheSignedUrl)
{
    const QDateTime fallback = QDateTime(QDate(2026, 10, 20), QTime(12, 0), QTimeZone::UTC);
    const QUrl signed_(u"https://cdnt-preview.dzcdn.net/x.mp3?hdnea=exp=1792000000~acl=/x*~hmac=ab"_s);
    EXPECT_EQ(DeezerClient::previewExpiry(signed_, fallback),
              QDateTime::fromSecsSinceEpoch(1792000000, QTimeZone::UTC));
    EXPECT_EQ(DeezerClient::previewExpiry(QUrl(u"https://cdnt-preview.dzcdn.net/x.mp3"_s), fallback), fallback);
    EXPECT_EQ(DeezerClient::previewExpiry(QUrl(u"https://x.test/a?exp=12"_s), fallback), fallback);
}

TEST(DeezerJson, QueriesAreCleaned)
{
    EXPECT_EQ(DeezerClient::cleanQuery(u"  artist:\"Kestrel Moon\"   track:\"Glass\" \n"_s),
              u"artist Kestrel Moon track Glass"_s);
    EXPECT_LE(DeezerClient::cleanQuery(QString(1000, u'x')).size(), 200);
    EXPECT_TRUE(DeezerClient::cleanQuery(u" \"\" : "_s).isEmpty());
}

TEST_F(DeezerClientTest, SearchSendsAPlainQuery)
{
    network.setDefault("GET", dzUrl(u"/search\\?"_s),
                       FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = wait(client->search(u"Kestrel Moon Glass Harbour"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().size(), 4);
    const QUrlQuery query(network.requests().first().url);
    EXPECT_EQ(query.queryItemValue(u"q"_s, QUrl::FullyDecoded), u"Kestrel Moon Glass Harbour"_s);
    EXPECT_EQ(query.queryItemValue(u"limit"_s), u"25"_s);
    ASSERT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.acquired().first(), selecta::governor::Operation::DeezerRequest);
    // A repeat is answered by the cache.
    ASSERT_TRUE(wait(client->search(u"Kestrel Moon Glass Harbour"_s))->ok());
    EXPECT_EQ(network.requestCount(), 1);
    // Bounds.
    ASSERT_TRUE(wait(client->search(u"other"_s, 500))->ok());
    EXPECT_EQ(QUrlQuery(network.requests().last().url).queryItemValue(u"limit"_s), u"50"_s);
    EXPECT_EQ(wait(client->search(u"  "_s))->error().code, ErrorCode::InvalidArgument);
}

TEST_F(DeezerClientTest, IsrcLookupFreshSkipsTheCache)
{
    network.setDefault("GET", dzUrl(u"/track/isrc:GBXYZ2600101$"_s),
                       FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    ASSERT_TRUE(wait(client->trackByIsrc(u"GBXYZ2600101"_s))->ok());
    ASSERT_TRUE(wait(client->trackByIsrc(u"GBXYZ2600101"_s))->ok());
    EXPECT_EQ(network.requestCount(), 1);
    const auto fresh = wait(client->trackByIsrc(u"gb-xyz-26-00101"_s, true));
    ASSERT_TRUE(fresh.has_value() && fresh->ok());
    EXPECT_EQ(network.requestCount(), 2);
    EXPECT_EQ(fresh->value().release.artists.size(), 2);
    EXPECT_EQ(wait(client->trackByIsrc(u"bad"_s))->error().code, ErrorCode::InvalidArgument);
}

TEST_F(DeezerClientTest, NoDataIsNotFoundAndNotCached)
{
    network.setDefault("GET", dzUrl(u"/track/isrc:"_s),
                       FakeResponse::json(R"({"error":{"type":"DataException","message":"no data","code":800}})"));
    EXPECT_EQ(wait(client->trackByIsrc(u"GBXYZ2699999"_s))->error().code, ErrorCode::NotFound);
    EXPECT_EQ(wait(client->trackByIsrc(u"GBXYZ2699999"_s))->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(DeezerClientTest, QuotaErrorsBackOffAndRetry)
{
    network.enqueue("GET", dzUrl(u"/track/3135553$"_s),
                    FakeResponse::json(R"({"error":{"type":"Exception","message":"Quota limit exceeded","code":4}})"));
    network.enqueue("GET", dzUrl(u"/track/3135553$"_s), FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    const auto result = wait(client->track(u"3135553"_s));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(network.requestCount(), 2);
    EXPECT_TRUE(pacer.records().first().outcome.contains(u"retrying"_s));
}

TEST_F(DeezerClientTest, OtherErrorsAndBadInput)
{
    network.enqueue("GET", dzUrl(u"/album/302127$"_s), FakeResponse::json(R"({"error":{"code":600,"message":"bad"}})"));
    EXPECT_EQ(wait(client->album(u"302127"_s))->error().code, ErrorCode::BadResponse);
    network.enqueue("GET", dzUrl(u"/album/302127$"_s), FakeResponse::json("{oops"));
    EXPECT_EQ(wait(client->album(u"302127"_s))->error().code, ErrorCode::BadResponse);
    network.enqueue("GET", dzUrl(u"/album/302127$"_s), FakeResponse::json(fixture(u"deezer/album.json"_s)));
    const auto album = wait(client->album(u"302127"_s));
    ASSERT_TRUE(album.has_value() && album->ok());
    EXPECT_EQ(album->value().label, u"Lantern Grove Records"_s);
    EXPECT_EQ(wait(client->album(u"../1"_s))->error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(wait(client->track(u"-5"_s))->error().code, ErrorCode::InvalidArgument);
}
