// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/CoverFetcher.h"

#include "meta/ServicePolicy.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"
#include "meta/support/Images.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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
using selecta::meta::testing::header;
using selecta::meta::testing::jpeg;
using selecta::meta::testing::png;

namespace {

const QString kReleaseMbid = u"c6a53877-7733-4bdb-9721-0dff076ce2ef"_s;
const QString kGroupMbid = u"0d464138-a623-4255-bfc1-ea36f17fd374"_s;
const QString kSpotifyCover = u"https://i.scdn.co/image/test-glass-harbour-640"_s;

FakeResponse image(const QByteArray &bytes, const QByteArray &type = "image/jpeg")
{
    FakeResponse response;
    response.body = bytes;
    response.headers.append({QByteArrayLiteral("Content-Type"), type});
    return response;
}

Release releaseWithCover(const QString &url = kSpotifyCover)
{
    Release release;
    release.title = u"Glass Harbour"_s;
    release.covers = {{QUrl(url), 640, 640}, {QUrl(u"https://i.scdn.co/image/test-300"_s), 300, 300}};
    return release;
}

class CoverFetcherTest : public ::testing::Test
{
protected:
    void make(const QString &cacheDir = QString(), std::chrono::hours ttl = std::chrono::hours(24 * 30),
              qint64 maxCacheBytes = qint64{500} * 1024 * 1024)
    {
        CoverFetcher::Config config;
        config.cacheDir = cacheDir;
        config.ttl = ttl;
        config.maxCacheBytes = maxCacheBytes;
        config.coverArtArchiveUrl = QUrl(u"https://caa.test"_s);
        fetcher = std::make_unique<CoverFetcher>(config, &network, pacer, clock);
    }

    std::optional<Result<CoverImage>> fetch(const Release &release)
    {
        return awaitAdvancing(fetcher->fetch(release), clock, 1s, 8000);
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    std::unique_ptr<CoverFetcher> fetcher;
};

} // namespace

TEST(CoverInspect, AcceptsJpegAndPngWithinBounds)
{
    const Result<CoverImage> jpegImage = CoverFetcher::inspect(jpeg(640, 640), QUrl(kSpotifyCover), Source::Spotify);
    ASSERT_TRUE(jpegImage.ok());
    EXPECT_EQ(jpegImage.value().mimeType, u"image/jpeg"_s);
    EXPECT_EQ(jpegImage.value().width, 640);
    EXPECT_EQ(jpegImage.value().source, Source::Spotify);
    const Result<CoverImage> pngImage = CoverFetcher::inspect(png(1000, 800), QUrl(), Source::CoverArtArchive);
    ASSERT_TRUE(pngImage.ok());
    EXPECT_EQ(pngImage.value().mimeType, u"image/png"_s);
    EXPECT_EQ(pngImage.value().height, 800);
    EXPECT_TRUE(CoverFetcher::inspect(jpeg(kMaxCoverPixels, kMaxCoverPixels), QUrl(), Source::Spotify).ok());
}

TEST(CoverInspect, RefusesHugeAndForeignImages)
{
    EXPECT_EQ(CoverFetcher::inspect(jpeg(kMaxCoverPixels + 1, 10), QUrl(), Source::Spotify).error().code,
              ErrorCode::TooLarge);
    EXPECT_EQ(CoverFetcher::inspect(png(10, 60'000), QUrl(), Source::Spotify).error().code, ErrorCode::TooLarge);
    EXPECT_EQ(CoverFetcher::inspect("<html>not found</html>", QUrl(), Source::Spotify).error().code,
              ErrorCode::UnsupportedFormat);
    EXPECT_EQ(CoverFetcher::inspect(QByteArray("GIF89a", 6), QUrl(), Source::Spotify).error().code,
              ErrorCode::UnsupportedFormat);
    QByteArray big = jpeg(100, 100);
    big += QByteArray(kMaxCoverBytes, '\0');
    EXPECT_EQ(CoverFetcher::inspect(big, QUrl(), Source::Spotify).error().code, ErrorCode::TooLarge);
}

TEST(CoverInspect, OnlyTheServicesCdnsAreAllowed)
{
    EXPECT_TRUE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://i.scdn.co/image/abc"_s)));
    EXPECT_TRUE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://mosaic.scdn.co/640/abc"_s)));
    EXPECT_TRUE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://image-cdn-ak.spotifycdn.com/image/abc"_s)));
    EXPECT_TRUE(
        CoverFetcher::isAllowedCoverUrl(QUrl(u"https://e-cdns-images.dzcdn.net/images/cover/x/1000x1000.jpg"_s)));
    EXPECT_FALSE(CoverFetcher::isAllowedCoverUrl(QUrl(u"http://i.scdn.co/image/abc"_s)));
    EXPECT_FALSE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://scdn.co.evil.example/x"_s)));
    EXPECT_FALSE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://192.168.1.1/x.jpg"_s)));
    EXPECT_FALSE(CoverFetcher::isAllowedCoverUrl(QUrl(u"https://evilscdn.co/x"_s)));
}

TEST_F(CoverFetcherTest, TheLargestLinkedImageComesFirst)
{
    make();
    network.enqueue("GET", u"^https://i\\.scdn\\.co/image/test-glass-harbour-640$"_s, image(jpeg(640, 640)));
    const auto result = fetch(releaseWithCover());
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().width, 640);
    EXPECT_EQ(result->value().source, Source::Spotify);
    EXPECT_EQ(result->value().url, QUrl(kSpotifyCover));
    EXPECT_EQ(network.requestCount(), 1);
    ASSERT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.acquired().first(), selecta::governor::Operation::CoverArtRequest);
}

TEST_F(CoverFetcherTest, DeezerCoversAreLabelledDeezer)
{
    make();
    const QString deezer = u"https://e-cdns-images.dzcdn.net/images/cover/abc/1000x1000-000000-80-0-0.jpg"_s;
    network.enqueue("GET", u"dzcdn"_s, image(jpeg(1000, 1000)));
    const auto result = fetch(releaseWithCover(deezer));
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().source, Source::Deezer);
}

TEST_F(CoverFetcherTest, FallsBackToTheCoverArtArchiveFollowingItsRedirect)
{
    make();
    network.enqueue("GET", u"i\\.scdn\\.co"_s, FakeResponse::json("{}", 404));
    FakeResponse moved;
    moved.status = 307;
    moved.headers.append(header("Location", "https://archive.test/download/mbid-x/front-1200.jpg"));
    network.enqueue("GET", u"^https://caa\\.test/release/"_s + kReleaseMbid + u"/front-1200$"_s, moved);
    network.enqueue("GET", u"^https://archive\\.test/"_s, image(jpeg(1200, 1200)));
    Release release = releaseWithCover();
    release.musicBrainzReleaseId = kReleaseMbid;
    const auto result = fetch(release);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().source, Source::CoverArtArchive);
    EXPECT_EQ(result->value().width, 1200);
    EXPECT_EQ(network.requestCount(), 3);
}

TEST_F(CoverFetcherTest, ReleaseGroupIsTheLastResort)
{
    make();
    network.enqueue("GET", u"/release/"_s, FakeResponse::json("{}", 404));
    network.enqueue("GET", u"^https://caa\\.test/release-group/"_s + kGroupMbid + u"/front-1200$"_s,
                    image(png(1200, 1200), "image/png"));
    Release release;
    release.musicBrainzReleaseId = kReleaseMbid;
    release.musicBrainzReleaseGroupId = kGroupMbid;
    const auto result = fetch(release);
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().mimeType, u"image/png"_s);
}

TEST_F(CoverFetcherTest, ForeignHostsAreSkipped)
{
    make();
    network.enqueue("GET", u"/release/"_s, image(jpeg(500, 500)));
    Release release = releaseWithCover(u"https://intranet.example/cover.jpg"_s);
    release.musicBrainzReleaseId = kReleaseMbid;
    const auto result = fetch(release);
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(result->value().source, Source::CoverArtArchive);
    ASSERT_EQ(network.requestCount(), 1);
    EXPECT_EQ(network.requests().first().url.host(), u"caa.test"_s);
}

TEST_F(CoverFetcherTest, OversizedAndBrokenImagesFallThrough)
{
    make();
    FakeResponse huge = image(jpeg(640, 640));
    huge.contentLength = kMaxCoverBytes + 1;
    network.enqueue("GET", u"i\\.scdn\\.co"_s, huge);
    network.enqueue("GET", u"/release/"_s, image("<html>oops</html>", "text/html"));
    network.enqueue("GET", u"/release-group/"_s, image(jpeg(20'000, 20'000)));
    Release release = releaseWithCover();
    release.musicBrainzReleaseId = kReleaseMbid;
    release.musicBrainzReleaseGroupId = kGroupMbid;
    const auto result = fetch(release);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::TooLarge); // the last source's error: 20 000 px
    EXPECT_EQ(network.requestCount(), 3);
}

TEST_F(CoverFetcherTest, NoSourceIsNotFound)
{
    make();
    const auto result = fetch(Release{});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->error().code, ErrorCode::NotFound);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(CoverFetcherTest, DiskCacheServesRepeatsAndExpires)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    make(dir.filePath(u"covers"_s));
    network.setDefault("GET", u"i\\.scdn\\.co"_s, image(jpeg(640, 640)));
    ASSERT_TRUE(fetch(releaseWithCover())->ok());
    ASSERT_TRUE(selecta::meta::testing::spinUntil(
        [&] { return QDir(dir.filePath(u"covers"_s)).entryList({u"*.img"_s}, QDir::Files).size() == 1; }));
    const auto cached = fetch(releaseWithCover());
    ASSERT_TRUE(cached.has_value() && cached->ok());
    EXPECT_EQ(cached->value().width, 640);
    EXPECT_EQ(network.requestCount(), 1); // served from the folder

    make(dir.filePath(u"covers"_s), std::chrono::hours(0)); // everything already expired
    ASSERT_TRUE(fetch(releaseWithCover())->ok());
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(CoverFetcherTest, CorruptCacheFilesAreIgnored)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString covers = dir.filePath(u"covers"_s);
    make(covers);
    network.setDefault("GET", u"i\\.scdn\\.co"_s, image(jpeg(640, 640)));
    ASSERT_TRUE(fetch(releaseWithCover())->ok());
    ASSERT_TRUE(selecta::meta::testing::spinUntil(
        [&] { return QDir(covers).entryList({u"*.json"_s}, QDir::Files).size() == 1; }));
    const QStringList images = QDir(covers).entryList({u"*.img"_s}, QDir::Files);
    for (const QString &name : images) {
        QFile file(QDir(covers).filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("garbage, not an image");
    }
    const auto result = fetch(releaseWithCover());
    ASSERT_TRUE(result.has_value() && result->ok());
    EXPECT_EQ(network.requestCount(), 2); // the broken copy was refetched
}

TEST_F(CoverFetcherTest, CacheFolderStaysUnderItsBound)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString covers = dir.filePath(u"covers"_s);
    const QByteArray bytes = jpeg(640, 640, 0xC0, 10'000); // ~10 kB each
    make(covers, std::chrono::hours(24), 25'000);          // room for two
    network.setDefault("GET", u"i\\.scdn\\.co"_s, image(bytes));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(fetch(releaseWithCover(u"https://i.scdn.co/image/cover-%1"_s.arg(i)))->ok());
        QTest::qWait(20);
    }
    ASSERT_TRUE(selecta::meta::testing::spinUntil(
        [&] { return QDir(covers).entryList({u"*.img"_s}, QDir::Files).size() <= 2; }));
}

TEST_F(CoverFetcherTest, AnUnusableCacheFolderJustDisablesTheCache)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile blocker(dir.filePath(u"file"_s));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    make(dir.filePath(u"file/covers"_s)); // a file stands where the folder should be
    network.setDefault("GET", u"i\\.scdn\\.co"_s, image(jpeg(640, 640)));
    ASSERT_TRUE(fetch(releaseWithCover())->ok());
    ASSERT_TRUE(fetch(releaseWithCover())->ok());
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(CoverFetcherTest, CancellingStopsTheDownload)
{
    make();
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", u"i\\.scdn\\.co"_s, hang);
    QFuture<Result<CoverImage>> future = fetcher->fetch(releaseWithCover());
    ASSERT_TRUE(selecta::meta::testing::spinUntil([&] { return network.requestCount() == 1; }));
    future.cancel();
    ASSERT_TRUE(selecta::meta::testing::spinUntil([&] { return network.liveReplies() == 0; }));
    EXPECT_TRUE(future.isCanceled());
}
