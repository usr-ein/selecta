// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/ClipSource.h"

#include "meta/DeezerClient.h"
#include "meta/MetaCache.h"
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
using selecta::base::ManualClock;
using selecta::meta::testing::awaitAdvancing;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::fixture;
using selecta::meta::testing::spinUntil;

namespace {

const QString kIsrcUrl = u"^https://dz\\.test/track/isrc:GBXYZ2600101$"_s;
const QString kTrackUrl = u"^https://dz\\.test/track/3135553$"_s;
const QString kSearchUrl = u"^https://dz\\.test/search\\?"_s;

FakeResponse noData()
{
    return FakeResponse::json(R"({"error":{"type":"DataException","message":"no data","code":800}})");
}

class ClipSourceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        deezer = std::make_unique<DeezerClient>(&network, pacer, &cache, clock, nullptr, QUrl(u"https://dz.test"_s));
        clips = std::make_unique<ClipSource>(*deezer);
        release.source = Source::Spotify;
        release.title = u"Glass Harbour - Extended Mix"_s;
        release.artists = {u"Kestrel Moon"_s};
        release.durationMs = 391000;
        release.isrc = u"GBXYZ2600101"_s;
        release.spotifyId = u"u8jzPde0IgxLd6GncfBAep"_s;
    }

    std::optional<Result<Clip>> clipFor(const Release &r) { return awaitAdvancing(clips->clipFor(r), clock, 1s, 8000); }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    std::unique_ptr<DeezerClient> deezer;
    std::unique_ptr<ClipSource> clips;
    Release release;
};

} // namespace

TEST_F(ClipSourceTest, ByIsrcWithAFreshSignedUrlEveryTime)
{
    network.setDefault("GET", kIsrcUrl, FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    const auto result = clipFor(release);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    const Clip &clip = result->value();
    EXPECT_TRUE(clip.url.toString().startsWith(u"https://cdnt-preview.dzcdn.net/"_s));
    EXPECT_EQ(clip.expiresAt, QDateTime::fromSecsSinceEpoch(1792000000, QTimeZone::UTC));
    EXPECT_EQ(clip.pageUrl, QUrl(u"https://www.deezer.com/track/3135553"_s));
    EXPECT_EQ(clip.deezerTrackId, u"3135553"_s);
    EXPECT_EQ(clip.source, Source::Deezer);
    EXPECT_EQ(clip.durationSec, 30);
    ASSERT_TRUE(clipFor(release)->ok());
    EXPECT_EQ(network.requestCount(), 2); // signed URLs expire: never from the cache
}

TEST_F(ClipSourceTest, AKnownDeezerIdComesFirst)
{
    release.deezerId = u"3135553"_s;
    network.enqueue("GET", kTrackUrl, FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    ASSERT_TRUE(clipFor(release)->ok());
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(ClipSourceTest, AStaleDeezerIdFallsBackToTheIsrc)
{
    release.deezerId = u"3135553"_s;
    network.enqueue("GET", kTrackUrl, noData());
    network.enqueue("GET", kIsrcUrl, FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    ASSERT_TRUE(clipFor(release)->ok());
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(ClipSourceTest, WithoutAnIsrcASearchFindsTheVersionThenSignsAfresh)
{
    release.isrc.clear();
    network.enqueue("GET", kSearchUrl, FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    network.enqueue("GET", kTrackUrl, FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    const auto result = clipFor(release);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().deezerTrackId, u"3135553"_s);
    ASSERT_EQ(network.requestCount(), 2);
    EXPECT_EQ(QUrlQuery(network.requests().first().url).queryItemValue(u"q"_s, QUrl::FullyDecoded),
              u"Kestrel Moon Glass Harbour"_s);
}

TEST_F(ClipSourceTest, AnUnknownIsrcFallsBackToTheSearch)
{
    network.enqueue("GET", kIsrcUrl, noData());
    network.enqueue("GET", kSearchUrl, FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    network.enqueue("GET", kTrackUrl, FakeResponse::json(fixture(u"deezer/track_isrc.json"_s)));
    ASSERT_TRUE(clipFor(release)->ok());
    EXPECT_EQ(network.requestCount(), 3);
}

TEST_F(ClipSourceTest, NoClipOfThisVersionIsNotFound)
{
    release.isrc.clear();
    release.title = u"Glass Harbour (Dub)"_s;
    network.enqueue("GET", kSearchUrl, FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    const auto result = clipFor(release);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 1);
    // Nor a hit of the right version whose length is off.
    release.title = u"Glass Harbour (Extended Mix)"_s;
    release.durationMs = 420000;
    network.enqueue("GET", kSearchUrl, FakeResponse::json(fixture(u"deezer/search_glass_harbour.json"_s)));
    EXPECT_EQ(clipFor(release)->error().code, ErrorCode::NotFound);
}

TEST_F(ClipSourceTest, ATrackWithoutAPreviewIsNotFound)
{
    QJsonObject track = QJsonDocument::fromJson(fixture(u"deezer/track_isrc.json"_s)).object();
    track.remove("preview"_L1);
    network.enqueue("GET", kIsrcUrl, FakeResponse::json(QJsonDocument(track).toJson()));
    EXPECT_EQ(clipFor(release)->error().code, ErrorCode::NotFound);
}

TEST_F(ClipSourceTest, AnUnsignedUrlGetsAShortLifetime)
{
    QJsonObject track = QJsonDocument::fromJson(fixture(u"deezer/track_isrc.json"_s)).object();
    track.insert("preview"_L1, u"https://cdnt-preview.dzcdn.net/api/1/1/a/b/c/0/abc123.mp3"_s);
    network.enqueue("GET", kIsrcUrl, FakeResponse::json(QJsonDocument(track).toJson()));
    const QDateTime before = QDateTime::currentDateTimeUtc();
    const auto result = clipFor(release);
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_GE(result->value().expiresAt, before.addSecs(qint64{9} * 60));
    EXPECT_LE(result->value().expiresAt, QDateTime::currentDateTimeUtc().addSecs((qint64{10} * 60) + 1));
}

TEST_F(ClipSourceTest, ErrorsOtherThanNotFoundAreReported)
{
    FakeResponse refused;
    refused.networkError = QNetworkReply::ConnectionRefusedError;
    network.enqueue("GET", kIsrcUrl, refused);
    EXPECT_EQ(clipFor(release)->error().code, ErrorCode::Network);
    EXPECT_EQ(network.requestCount(), 1); // no search after a failure
    const Release empty;
    EXPECT_EQ(clipFor(empty)->error().code, ErrorCode::NotFound);
}

TEST_F(ClipSourceTest, CancellingAbortsTheRequest)
{
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", kIsrcUrl, hang);
    QFuture<Result<Clip>> future = clips->clipFor(release);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    future.cancel();
    ASSERT_TRUE(spinUntil([&] { return network.liveReplies() == 0; }));
    EXPECT_TRUE(future.isCanceled());
}
