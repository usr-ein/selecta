// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/HttpChannel.h"

#include "meta/MetaCache.h"
#include "meta/detail/ServiceGate.h"
#include "meta/support/Await.h"
#include "meta/support/FakeNetwork.h"
#include "meta/support/FakePacer.h"

#include <array>
#include <gtest/gtest.h>
#include <memory>
#include <optional>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::meta;
using namespace selecta::meta::detail;
using selecta::base::ManualClock;
using selecta::governor::Operation;
using selecta::meta::testing::FakeNetwork;
using selecta::meta::testing::FakePacer;
using selecta::meta::testing::FakeResponse;
using selecta::meta::testing::header;
using selecta::meta::testing::spinUntil;

namespace {

class HttpChannelTest : public ::testing::Test
{
protected:
    void makeChannel(Service service)
    {
        channel = std::make_unique<HttpChannel>(service, &network, pacer, &cache, clock);
    }

    // Sends and collects the result in `result`.
    HttpChannel::RequestId get(const QString &url, HttpRequest base = {})
    {
        base.url = QUrl(url);
        result.reset();
        calls = 0;
        return channel->send(std::move(base), [this](Result<HttpResponse> r) {
            ++calls;
            result.emplace(std::move(r));
        });
    }

    bool waitResult(int ms = 3000)
    {
        return spinUntil([this] { return result.has_value(); }, ms);
    }

    // Waits while moving the clock, for flows that cross the gate's spacing (redirect hops).
    bool waitAdvancing(int ms = 5000)
    {
        return spinUntil(
            [this] {
                advance(1s);
                return result.has_value();
            },
            ms);
    }

    // Moves the clock and lets the gate see it.
    void advance(std::chrono::milliseconds by)
    {
        clock.advance(by);
        ServiceGate::pollAll();
    }

    ManualClock clock;
    FakeNetwork network;
    FakePacer pacer;
    MemoryMetaCache cache{clock};
    std::unique_ptr<HttpChannel> channel;
    std::optional<Result<HttpResponse>> result;
    int calls = 0;
};

} // namespace

TEST_F(HttpChannelTest, GetSucceedsWithOnePermitAndTheUserAgent)
{
    makeChannel(Service::SpotifyApi);
    network.enqueue("GET", u"^https://api\\.test/v1/x$"_s, FakeResponse::json(R"({"ok":true})"));
    get(u"https://api.test/v1/x"_s);
    EXPECT_FALSE(result.has_value()); // never inside send()
    ASSERT_TRUE(waitResult());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(result->value().status, 200);
    EXPECT_EQ(result->value().body, R"({"ok":true})");
    EXPECT_EQ(result->value().header("content-type"), "application/json");
    ASSERT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.acquired().first(), Operation::SpotifyRequest);
    ASSERT_EQ(network.requestCount(), 1);
    EXPECT_TRUE(network.requests().first().header("User-Agent").startsWith("Selecta/"));
    EXPECT_TRUE(network.requests().first().header("User-Agent").contains("https://github.com/usr-ein/selecta"));
    ASSERT_EQ(pacer.records().size(), 1);
    EXPECT_EQ(pacer.records().first().outcome, u"ok"_s);
    EXPECT_EQ(pacer.records().first().httpStatus, 200);
}

TEST_F(HttpChannelTest, EachServiceAsksForItsOperation)
{
    const std::array<std::pair<Service, Operation>, 5> cases = {
        {{Service::MusicBrainz, Operation::MusicBrainzRequest},
         {Service::Deezer, Operation::DeezerRequest},
         {Service::CoverArtArchive, Operation::CoverArtRequest},
         {Service::CoverImages, Operation::CoverArtRequest},
         {Service::SpotifyAccounts, Operation::SpotifyRequest}}};
    for (const auto &[service, operation] : cases) {
        FakePacer local;
        HttpChannel c(service, &network, local, nullptr, clock);
        network.enqueue("GET", u"^https://svc\\.test/"_s, FakeResponse::json("{}"));
        bool done = false;
        c.send(HttpRequest{.url = QUrl(u"https://svc.test/"_s)}, [&](const Result<HttpResponse> &) { done = true; });
        ASSERT_TRUE(spinUntil([&] { return done; }));
        ASSERT_EQ(local.acquiredCount(), 1);
        EXPECT_EQ(local.acquired().first(), operation);
    }
}

TEST_F(HttpChannelTest, RefusedPermitFailsWithoutSending)
{
    makeChannel(Service::Deezer);
    pacer.refuse(u"Network activity is paused"_s);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Refused);
    EXPECT_EQ(result->error().message, u"Network activity is paused"_s);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(HttpChannelTest, ShortRefusalWaitsAndAsksAgain)
{
    makeChannel(Service::Deezer);
    pacer.refuse(u"budget spent"_s, 2s, 1);
    network.enqueue("GET", u"/x$"_s, FakeResponse::json("{}"));
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return pacer.acquiredCount() == 1; }));
    QTest::qWait(20);
    EXPECT_FALSE(result.has_value());
    advance(2s);
    ASSERT_TRUE(waitResult());
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(pacer.acquiredCount(), 2);
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(HttpChannelTest, LongRefusalFailsWithRetryAfter)
{
    makeChannel(Service::Deezer); // maxWait 30 s
    pacer.refuse(u"budget spent"_s, 120s, 1);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Refused);
    EXPECT_EQ(result->error().retryAfter, 120s);
}

TEST_F(HttpChannelTest, RetryAfterIsHonouredThenRetried)
{
    makeChannel(Service::SpotifyApi);
    FakeResponse limited = FakeResponse::json("{}", 429);
    limited.headers.append(header("Retry-After", "2"));
    network.enqueue("GET", u"/x$"_s, limited);
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"second":1})"));
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    QTest::qWait(20);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(channel->blockRemaining(), 2s);
    advance(1999ms);
    QTest::qWait(10);
    EXPECT_EQ(network.requestCount(), 1);
    advance(1ms);
    ASSERT_TRUE(waitResult());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(result->value().body, R"({"second":1})");
    EXPECT_EQ(pacer.acquiredCount(), 2); // a retry is a new request: a new permit
    EXPECT_TRUE(pacer.records().first().outcome.contains(u"retrying"_s));
}

TEST_F(HttpChannelTest, LongRetryAfterFailsAtOnceAndBlocksLaterRequests)
{
    makeChannel(Service::SpotifyApi);
    FakeResponse limited = FakeResponse::json("{}", 429);
    limited.headers.append(header("Retry-After", "86400"));
    network.enqueue("GET", u"/x$"_s, limited);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(result->error().retryAfter, 86400s);
    EXPECT_EQ(result->error().httpStatus, 429);
    // The next request fails fast without asking the governor or the network.
    get(u"https://api.test/y"_s);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(result->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(HttpChannelTest, MusicBrainz503BacksOffDoublingThenGivesUp)
{
    makeChannel(Service::MusicBrainz); // 2 s, ×2, 2 retries
    for (int i = 0; i < 3; ++i) {
        network.enqueue("GET", u"/ws$"_s, FakeResponse::json("{}", 503));
    }
    get(u"https://mb.test/ws"_s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    QTest::qWait(10);
    EXPECT_EQ(channel->blockRemaining(), 2s);
    advance(2s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 2; }));
    QTest::qWait(10);
    EXPECT_EQ(channel->blockRemaining(), 4s);
    advance(4s);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(network.requestCount(), 3); // at most 3 tries in all
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::RateLimited);
    EXPECT_EQ(result->error().httpStatus, 503);
}

TEST_F(HttpChannelTest, ServerErrorRetriesThenSucceeds)
{
    makeChannel(Service::Deezer);
    network.enqueue("GET", u"/x$"_s, FakeResponse::json("{}", 502));
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"ok":1})"));
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    QTest::qWait(10);
    EXPECT_EQ(channel->blockRemaining(), 5s); // Deezer's first back-off, for the whole service
    advance(5s);
    ASSERT_TRUE(waitResult());
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(HttpChannelTest, TransportErrorIsANetworkError)
{
    makeChannel(Service::Deezer);
    FakeResponse refused;
    refused.networkError = QNetworkReply::ConnectionRefusedError;
    network.enqueue("GET", u"/x$"_s, refused);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Network);
}

TEST_F(HttpChannelTest, AHangingRequestTimesOut)
{
    makeChannel(Service::Deezer);
    channel->setRequestTimeout(100ms);
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", u"/x$"_s, hang);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Timeout);
    EXPECT_EQ(network.liveReplies(), 0);
}

TEST_F(HttpChannelTest, AnnouncedOversizedBodyIsAborted)
{
    makeChannel(Service::SpotifyAccounts); // 1 MiB bound
    FakeResponse big = FakeResponse::json("{}");
    big.contentLength = kMiB + 1;
    network.enqueue("POST", u"/token$"_s, big);
    HttpRequest request;
    request.method = "POST";
    get(u"https://acc.test/token"_s, request);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::TooLarge);
}

TEST_F(HttpChannelTest, StreamedOversizedBodyIsAborted)
{
    makeChannel(Service::SpotifyAccounts);
    FakeResponse big = FakeResponse::json(QByteArray(kMiB + 10, 'x'));
    big.contentLength = 0; // lies: says nothing useful
    big.chunkSize = 64 * 1024;
    network.enqueue("GET", u"/big$"_s, big);
    get(u"https://acc.test/big"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::TooLarge);
}

TEST_F(HttpChannelTest, ChunkedBodyIsReassembled)
{
    makeChannel(Service::Deezer);
    QByteArray body;
    for (int i = 0; i < 1000; ++i) {
        body += QByteArray::number(i) + ',';
    }
    FakeResponse chunked = FakeResponse::json(body);
    chunked.chunkSize = 97;
    network.enqueue("GET", u"/x$"_s, chunked);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(result->value().body, body);
}

TEST_F(HttpChannelTest, RedirectIsFollowedWithANewPermitAndNoAuthorizationAcrossHosts)
{
    makeChannel(Service::CoverArtArchive);
    FakeResponse moved;
    moved.status = 307;
    moved.headers.append(header("Location", "https://archive.test/download/front.jpg"));
    network.enqueue("GET", u"^https://caa\\.test/release/1/front$"_s, moved);
    FakeResponse image;
    image.body = "JPEGDATA";
    network.enqueue("GET", u"^https://archive\\.test/download/front\\.jpg$"_s, image);
    HttpRequest request;
    request.headers.append(header("Authorization", "Bearer secret"));
    get(u"https://caa.test/release/1/front"_s, request);
    ASSERT_TRUE(waitAdvancing());
    ASSERT_TRUE(result->ok()) << result->error().toString().toStdString();
    EXPECT_EQ(result->value().body, "JPEGDATA");
    EXPECT_EQ(result->value().url, QUrl(u"https://archive.test/download/front.jpg"_s));
    EXPECT_EQ(pacer.acquiredCount(), 2);
    ASSERT_EQ(network.requestCount(), 2);
    EXPECT_EQ(network.requests().at(0).header("Authorization"), "Bearer secret");
    EXPECT_TRUE(network.requests().at(1).header("Authorization").isEmpty());
}

TEST_F(HttpChannelTest, RelativeRedirectKeepsTheHost)
{
    makeChannel(Service::CoverArtArchive);
    FakeResponse moved;
    moved.status = 302;
    moved.headers.append(header("Location", "/elsewhere"));
    network.enqueue("GET", u"/start$"_s, moved);
    network.enqueue("GET", u"^https://caa\\.test/elsewhere$"_s, FakeResponse::json("{}"));
    get(u"https://caa.test/start"_s);
    ASSERT_TRUE(waitAdvancing());
    EXPECT_TRUE(result->ok());
}

TEST_F(HttpChannelTest, RedirectToPlainHttpIsRefused)
{
    makeChannel(Service::CoverArtArchive);
    FakeResponse moved;
    moved.status = 301;
    moved.headers.append(header("Location", "http://archive.test/front.jpg"));
    network.enqueue("GET", u"/start$"_s, moved);
    get(u"https://caa.test/start"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(HttpChannelTest, RedirectLoopStops)
{
    makeChannel(Service::CoverArtArchive);
    FakeResponse loop;
    loop.status = 302;
    loop.headers.append(header("Location", "https://caa.test/loop"));
    network.setDefault("GET", u"/loop$"_s, loop);
    get(u"https://caa.test/loop"_s);
    ASSERT_TRUE(selecta::meta::testing::spinUntil(
        [&] {
            advance(1s);
            return result.has_value();
        },
        5000));
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);
    EXPECT_EQ(network.requestCount(), policyFor(Service::CoverArtArchive).maxRedirects + 1);
}

TEST_F(HttpChannelTest, SeeOtherTurnsAPostIntoAGet)
{
    makeChannel(Service::CoverArtArchive);
    FakeResponse other;
    other.status = 303;
    other.headers.append(header("Location", "https://caa.test/result"));
    network.enqueue("POST", u"/submit$"_s, other);
    network.enqueue("GET", u"/result$"_s, FakeResponse::json("{}"));
    HttpRequest request;
    request.method = "POST";
    request.body = "a=1";
    get(u"https://caa.test/submit"_s, request);
    ASSERT_TRUE(waitAdvancing());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(network.requests().at(1).method, "GET");
    EXPECT_TRUE(network.requests().at(1).body.isEmpty());
}

TEST_F(HttpChannelTest, CacheAnswersRepeatsWithoutPermitOrNetwork)
{
    makeChannel(Service::Deezer);
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"v":1})"));
    HttpRequest request;
    request.cacheKey = u"deezer/x"_s;
    request.cacheTtl = 60s;
    get(u"https://api.test/x"_s, request);
    ASSERT_TRUE(waitResult());
    ASSERT_TRUE(result->ok());
    EXPECT_FALSE(result->value().fromCache);

    get(u"https://api.test/x"_s, request);
    EXPECT_FALSE(result.has_value()); // still asynchronous
    ASSERT_TRUE(waitResult());
    ASSERT_TRUE(result->ok());
    EXPECT_TRUE(result->value().fromCache);
    EXPECT_EQ(result->value().body, R"({"v":1})");
    EXPECT_EQ(network.requestCount(), 1);
    EXPECT_EQ(pacer.acquiredCount(), 1);
    EXPECT_EQ(pacer.records().last().outcome, u"cached"_s);

    clock.advance(61s); // expired: asks again
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"v":2})"));
    get(u"https://api.test/x"_s, request);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(result->value().body, R"({"v":2})");
    EXPECT_EQ(network.requestCount(), 2);
}

TEST_F(HttpChannelTest, JsonInspectionKeepsBrokenBodiesOutOfTheCache)
{
    makeChannel(Service::MusicBrainz);
    channel->setInspector([](const HttpResponse &response) { return jsonInspection(Service::MusicBrainz, response); });
    HttpRequest request;
    request.cacheKey = u"musicbrainz/x"_s;
    request.cacheTtl = 60s;
    network.enqueue("GET", u"/x$"_s, FakeResponse::json("<html>proxy error</html>"));
    get(u"https://api.test/x"_s, request);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);
    EXPECT_EQ(network.requestCount(), 1);                           // not retried
    network.enqueue("GET", u"/x$"_s, FakeResponse::json("[1, 2]")); // JSON, but not an object
    get(u"https://api.test/x"_s, request);
    ASSERT_TRUE(waitAdvancing());
    EXPECT_EQ(result->error().code, ErrorCode::BadResponse);
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"v":1})"));
    get(u"https://api.test/x"_s, request);
    ASSERT_TRUE(waitAdvancing());
    ASSERT_TRUE(result->ok());
    EXPECT_FALSE(result->value().fromCache);
    EXPECT_EQ(network.requestCount(), 3);
    // Other statuses are left to the caller, as with the default inspection.
    network.enqueue("GET", u"/y$"_s, FakeResponse::json("not json", 404));
    get(u"https://api.test/y"_s);
    ASSERT_TRUE(waitAdvancing());
    ASSERT_TRUE(result->ok());
    EXPECT_EQ(result->value().status, 404);
}

TEST_F(HttpChannelTest, NotFoundIsCachedOnlyWhenAsked)
{
    makeChannel(Service::MusicBrainz);
    network.setDefault("GET", u"/isrc/"_s, FakeResponse::json(R"({"error":"Not Found"})", 404));
    HttpRequest request;
    request.cacheKey = u"musicbrainz/isrc/X"_s;
    request.cacheTtl = 60s;
    request.notFoundTtl = 30s;
    get(u"https://mb.test/isrc/X"_s, request);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(result->value().status, 404);
    get(u"https://mb.test/isrc/X"_s, request);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(result->value().status, 404);
    EXPECT_TRUE(result->value().fromCache);
    EXPECT_EQ(network.requestCount(), 1);

    HttpRequest uncached = request;
    uncached.cacheKey = u"musicbrainz/isrc/Y"_s;
    uncached.notFoundTtl = 0s;
    advance(2s);
    get(u"https://mb.test/isrc/Y"_s, uncached);
    ASSERT_TRUE(waitResult());
    advance(2s);
    get(u"https://mb.test/isrc/Y"_s, uncached);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(network.requestCount(), 3);
}

TEST_F(HttpChannelTest, CancelledQueuedRequestNeverCallsBackNorSends)
{
    makeChannel(Service::MusicBrainz);
    network.setDefault("GET", u"/x$"_s, FakeResponse::json("{}"));
    get(u"https://mb.test/x"_s);
    ASSERT_TRUE(waitResult());
    // The next one has to wait 1.1 s for the gate.
    const auto id = get(u"https://mb.test/x"_s);
    QTest::qWait(10);
    channel->cancel(id);
    advance(2s);
    QTest::qWait(20);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(network.requestCount(), 1);
}

TEST_F(HttpChannelTest, CancelledInFlightRequestIsAbortedQuietly)
{
    makeChannel(Service::Deezer);
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", u"/x$"_s, hang);
    const auto id = get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    channel->cancel(id);
    QTest::qWait(20);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(network.liveReplies(), 0);
    EXPECT_EQ(channel->gate().inFlight(), 0);
}

TEST_F(HttpChannelTest, CancelWhileWaitingForThePermitDropsIt)
{
    makeChannel(Service::Deezer);
    pacer.setManual(true);
    const auto id = get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return pacer.pendingCount() == 1; }));
    channel->cancel(id);
    pacer.grantPending();
    QTest::qWait(20);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(network.requestCount(), 0);
    EXPECT_FALSE(channel->gate().hasReservation());
}

TEST_F(HttpChannelTest, DestroyingTheChannelWithWorkInFlightIsQuiet)
{
    makeChannel(Service::Deezer);
    FakeResponse hang;
    hang.hang = true;
    network.enqueue("GET", u"/x$"_s, hang);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return network.requestCount() == 1; }));
    channel.reset();
    QTest::qWait(20);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(network.liveReplies(), 0);
}

TEST_F(HttpChannelTest, LatePermitAfterDestructionIsHarmless)
{
    makeChannel(Service::Deezer);
    pacer.setManual(true);
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(spinUntil([&] { return pacer.pendingCount() == 1; }));
    channel.reset();
    pacer.grantPending(); // must not touch the destroyed channel
    QTest::qWait(10);
    EXPECT_EQ(network.requestCount(), 0);
}

TEST_F(HttpChannelTest, NonHttpsUrlsAreRefused)
{
    makeChannel(Service::Deezer);
    get(u"http://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::Internal);
    EXPECT_EQ(pacer.acquiredCount(), 0);
}

TEST_F(HttpChannelTest, InspectorCanFailAndBlockTheService)
{
    makeChannel(Service::SpotifyApi);
    channel->setInspector([](const HttpResponse &response) {
        RetryAdvice advice;
        if (response.body.contains("QUOTA")) {
            advice.kind = RetryAdvice::Kind::Fail;
            advice.delay = std::chrono::hours(1);
            advice.blockService = true;
            advice.error = makeError(Service::SpotifyApi, ErrorCode::QuotaExceeded, u"quota"_s);
        }
        return advice;
    });
    network.enqueue("GET", u"/x$"_s, FakeResponse::json(R"({"reason":"QUOTA"})", 429));
    get(u"https://api.test/x"_s);
    ASSERT_TRUE(waitResult());
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(result->error().retryAfter, 3600s);
    EXPECT_EQ(channel->blockRemaining(), 1h);
}

TEST_F(HttpChannelTest, RequestBodyAndHeadersReachTheServerButNotTheLog)
{
    makeChannel(Service::SpotifyAccounts);
    network.enqueue("POST", u"/api/token$"_s, FakeResponse::json("{}"));
    HttpRequest request;
    request.method = "POST";
    request.body = "grant_type=refresh_token&refresh_token=SECRET";
    request.headers.append(header("Authorization", "Basic SECRET"));
    request.logTarget = u"acc.test/api/token (refresh)"_s;
    get(u"https://acc.test/api/token"_s, request);
    ASSERT_TRUE(waitResult());
    EXPECT_EQ(network.requests().first().body, request.body);
    EXPECT_EQ(network.requests().first().header("Authorization"), "Basic SECRET");
    for (const auto &record : pacer.records()) {
        EXPECT_FALSE(record.target.contains(u"SECRET"_s));
        EXPECT_FALSE(record.outcome.contains(u"SECRET"_s));
        EXPECT_EQ(record.target, u"acc.test/api/token (refresh)"_s);
    }
}

TEST(HttpChannelHelpers, ParseRetryAfter)
{
    const QDateTime now = QDateTime(QDate(2026, 10, 20), QTime(12, 0), QTimeZone::UTC);
    EXPECT_EQ(parseRetryAfter("5", now), 5000ms);
    EXPECT_EQ(parseRetryAfter(" 0 ", now), 0ms);
    EXPECT_FALSE(parseRetryAfter("-3", now).has_value());
    EXPECT_FALSE(parseRetryAfter("soon", now).has_value());
    EXPECT_FALSE(parseRetryAfter("", now).has_value());
    EXPECT_EQ(parseRetryAfter("999999999999", now), std::chrono::hours(48)); // bounded
    EXPECT_EQ(parseRetryAfter("Tue, 20 Oct 2026 12:00:30 GMT", now), 30s);
    EXPECT_EQ(parseRetryAfter("Tue, 20 Oct 2026 11:00:00 GMT", now), 0ms); // in the past
}

TEST(HttpChannelHelpers, ScrubbedTargetHidesSecrets)
{
    const QString target = scrubbedTarget(QUrl(
        u"https://accounts.spotify.com/authorize?client_id=ABC&state=XYZ&code=CODE&scope=a%20b&code_challenge=CH"_s));
    EXPECT_FALSE(target.contains(u"ABC"_s));
    EXPECT_FALSE(target.contains(u"XYZ"_s));
    EXPECT_FALSE(target.contains(u"CODE"_s));
    EXPECT_FALSE(target.contains(u"=CH"_s));
    EXPECT_TRUE(target.contains(u"accounts.spotify.com/authorize"_s));
    EXPECT_TRUE(target.contains(u"scope"_s));
    EXPECT_FALSE(scrubbedTarget(QUrl(u"https://user:pass@x.test/p"_s)).contains(u"pass"_s));
}

TEST(HttpChannelHelpers, StatusErrors)
{
    HttpResponse response;
    const std::array<std::pair<int, ErrorCode>, 7> cases = {{{400, ErrorCode::InvalidArgument},
                                                             {401, ErrorCode::SignInRequired},
                                                             {403, ErrorCode::Forbidden},
                                                             {404, ErrorCode::NotFound},
                                                             {429, ErrorCode::RateLimited},
                                                             {500, ErrorCode::ServerError},
                                                             {418, ErrorCode::BadResponse}}};
    for (const auto &[status, code] : cases) {
        response.status = status;
        EXPECT_EQ(statusError(Service::Deezer, response).code, code) << status;
        EXPECT_EQ(statusError(Service::Deezer, response).httpStatus, status);
    }
}

TEST(HttpChannelHelpers, UserAgentHasNameVersionAndContact)
{
    const QByteArray agent = userAgent();
    EXPECT_TRUE(agent.startsWith("Selecta/"));
    EXPECT_TRUE(agent.endsWith("( https://github.com/usr-ein/selecta )"));
}
