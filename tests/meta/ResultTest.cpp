// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/Result.h"

#include "meta/Release.h"
#include "meta/ServicePolicy.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
using selecta::governor::Operation;

TEST(Result, HoldsAValue)
{
    const Result<int> result(42);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
}

TEST(Result, HoldsAnError)
{
    const Result<int> result(makeError(Service::Deezer, ErrorCode::NotFound, u"no such track"_s, 404));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
    EXPECT_EQ(result.error().service, Service::Deezer);
    EXPECT_EQ(result.error().httpStatus, 404);
}

TEST(Result, VoidSpecialisation)
{
    const Result<void> ok;
    EXPECT_TRUE(ok.ok());
    const Result<void> failed(makeError(Service::Keychain, ErrorCode::Internal, u"x"_s));
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.error().service, Service::Keychain);
}

TEST(Result, MovesTheValueOut)
{
    Result<QString> result(u"text"_s);
    const QString moved = std::move(result).value();
    EXPECT_EQ(moved, u"text"_s);
}

TEST(Result, ErrorToStringNamesServiceCodeStatusAndWait)
{
    MetaError error = makeError(Service::MusicBrainz, ErrorCode::RateLimited, u"slow down"_s, 503);
    error.retryAfter = std::chrono::seconds(4);
    const QString text = error.toString();
    EXPECT_TRUE(text.contains(u"MusicBrainz"_s)) << text.toStdString();
    EXPECT_TRUE(text.contains(u"rate limited"_s)) << text.toStdString();
    EXPECT_TRUE(text.contains(u"503"_s)) << text.toStdString();
    EXPECT_TRUE(text.contains(u"4 s"_s)) << text.toStdString();
    EXPECT_TRUE(text.contains(u"slow down"_s)) << text.toStdString();
}

TEST(Result, EveryNetworkServiceMapsToItsGovernorOperation)
{
    EXPECT_EQ(operationFor(Service::SpotifyApi), Operation::SpotifyRequest);
    EXPECT_EQ(operationFor(Service::SpotifyAccounts), Operation::SpotifyRequest);
    EXPECT_EQ(operationFor(Service::CoverImages), Operation::CoverArtRequest);
    EXPECT_EQ(operationFor(Service::CoverArtArchive), Operation::CoverArtRequest);
    EXPECT_EQ(operationFor(Service::Deezer), Operation::DeezerRequest);
    EXPECT_EQ(operationFor(Service::MusicBrainz), Operation::MusicBrainzRequest);
    EXPECT_EQ(operationFor(Service::YtDlp), Operation::YtDlpRun);
    EXPECT_FALSE(operationFor(Service::Keychain).has_value());
}

TEST(Result, NamesAreNeverEmpty)
{
    for (int i = 0; i <= static_cast<int>(ErrorCode::Internal); ++i) {
        EXPECT_FALSE(errorCodeName(static_cast<ErrorCode>(i)).isEmpty()) << i;
    }
    for (int i = 0; i <= static_cast<int>(Service::Keychain); ++i) {
        EXPECT_FALSE(serviceName(static_cast<Service>(i)).isEmpty()) << i;
    }
    for (int i = 0; i <= static_cast<int>(Source::YouTube); ++i) {
        EXPECT_FALSE(sourceName(static_cast<Source>(i)).isEmpty()) << i;
    }
}

TEST(ServicePolicy, PublishedLimitsHold)
{
    // MusicBrainz: 1 request/s on average, one at a time.
    EXPECT_GE(policyFor(Service::MusicBrainz).minSpacing, std::chrono::milliseconds(1000));
    EXPECT_EQ(policyFor(Service::MusicBrainz).maxInFlight, 1);
    // Deezer: at most 50 requests in 5 s.
    const ServicePolicy &deezer = policyFor(Service::Deezer);
    EXPECT_LE(qint64{deezer.windowCount} * 5000 / deezer.window.count(), 50);
    EXPECT_GE(deezer.minSpacing * 50,
              std::chrono::milliseconds(5000)); // even back-to-back starts stay under 50 per 5 s
    // yt-dlp: one process at a time, ≥ 2 s apart, ≤ 30 per 10 minutes.
    EXPECT_EQ(policyFor(Service::YtDlp).maxInFlight, 1);
    EXPECT_GE(policyFor(Service::YtDlp).minSpacing, std::chrono::milliseconds(2000));
    EXPECT_LE(policyFor(Service::YtDlp).windowCount, 30);
    // Covers are bounded to 20 MiB.
    EXPECT_EQ(policyFor(Service::CoverImages).maxBodyBytes, kMaxCoverBytes);
    EXPECT_EQ(policyFor(Service::CoverArtArchive).maxBodyBytes, kMaxCoverBytes);
}
