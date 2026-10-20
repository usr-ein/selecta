// SPDX-License-Identifier: GPL-3.0-or-later
// Length windows (note §7, PRD D23) and quality ranks (note §9, PRD SRCH-6/7, LIB-4).
#include "MatchTestSupport.h"
#include "match/Duration.h"
#include "match/Quality.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::Codec;
using selecta::match::DurationComparison;
using selecta::match::RemoteAttributes;
using selecta::match::RemoteQuality;

namespace {

TEST(Duration, WindowsScaleWithTheLocalLength) // note §7, T17
{
    const DurationComparison d = selecta::match::compareRemoteDuration(363, 360400);
    ASSERT_TRUE(d.known);
    EXPECT_DOUBLE_EQ(d.delta, 3.0); // |363 − floor(360.4)|
    EXPECT_DOUBLE_EQ(d.exactWindow, 3.0);
    EXPECT_NEAR(d.likelyWindow, 0.03 * 360.4, 1e-9);
    EXPECT_NEAR(d.farWindow, 0.08 * 360.4, 1e-9);
    EXPECT_DOUBLE_EQ(d.vetoWindow, 90.1);
    EXPECT_TRUE(d.withinExact());
    EXPECT_DOUBLE_EQ(d.score, 1.0);

    const DurationComparison four = selecta::match::compareRemoteDuration(364, 360400);
    EXPECT_FALSE(four.withinExact());
    EXPECT_NEAR(four.score, 1.0 - (0.5 * 1.0 / ((0.03 * 360.4) - 3.0)), 1e-9);
}

TEST(Duration, ScoreFallsToZeroAndVetoes) // note §7
{
    const DurationComparison far = selecta::match::compareRemoteDuration(385, 360000); // T18: Δ 25
    EXPECT_TRUE(far.withinLikely() == false);
    EXPECT_FALSE(far.beyondFar());
    EXPECT_NEAR(far.score, 0.5 * (28.8 - 25.0) / (28.8 - 10.8), 1e-9);
    const DurationComparison beyond = selecta::match::compareRemoteDuration(400, 360000);
    EXPECT_TRUE(beyond.beyondFar());
    EXPECT_DOUBLE_EQ(beyond.score, 0.0);
    EXPECT_FALSE(beyond.vetoed());
    const DurationComparison veto = selecta::match::compareRemoteDuration(392, 269000); // T03
    EXPECT_TRUE(veto.vetoed());
    // Short tracks use the minimum windows: 6 s, 15 s, 60 s.
    const DurationComparison shortTrack = selecta::match::compareRemoteDuration(80, 72000);
    EXPECT_DOUBLE_EQ(shortTrack.likelyWindow, 6.0);
    EXPECT_DOUBLE_EQ(shortTrack.farWindow, 15.0);
    EXPECT_DOUBLE_EQ(shortTrack.vetoWindow, 60.0);
}

TEST(Duration, TruncationIsFree) // Nicotine+ truncates lengths: remote ∈ [d − 1, d]
{
    EXPECT_DOUBLE_EQ(selecta::match::compareRemoteDuration(269, 269999).delta, 0.0);
    EXPECT_DOUBLE_EQ(selecta::match::compareRemoteDuration(266, 269999).delta, 3.0);
}

TEST(Duration, MissingLengthsAreUnknown) // a length of 0 is absent
{
    EXPECT_FALSE(selecta::match::compareRemoteDuration(0, 300000).known);
    EXPECT_FALSE(selecta::match::compareRemoteDuration(300, 0).known);
    EXPECT_FALSE(selecta::match::compareRemoteDuration(-5, 300000).known);
    EXPECT_FALSE(selecta::match::compareDurations(0, 300000).known);
    const DurationComparison unknown = selecta::match::compareRemoteDuration(0, 0);
    EXPECT_FALSE(unknown.withinExact());
    EXPECT_FALSE(unknown.withinLikely());
    EXPECT_FALSE(unknown.beyondFar());
    EXPECT_FALSE(unknown.vetoed());
}

TEST(Duration, LocalLengthsCompareExactly) // supersede: Δ in milliseconds
{
    const DurationComparison d = selecta::match::compareDurations(269400, 269000);
    EXPECT_NEAR(d.delta, 0.4, 1e-9);
    EXPECT_TRUE(selecta::match::isMixLength(1201));
    EXPECT_FALSE(selecta::match::isMixLength(1200));
}

TEST(Quality, RanksFollowTheNote) // note §9
{
    using selecta::match::qualityRank;
    EXPECT_EQ(qualityRank({Codec::Flac, 0, std::nullopt, 96000, 24}), 6);
    EXPECT_EQ(qualityRank({Codec::Flac, 0, std::nullopt, 44100, 16}), 5);
    EXPECT_EQ(qualityRank({Codec::Wav, 0, std::nullopt, 0, 0}), 5); // lossless without a bit depth: 16-bit
    EXPECT_EQ(qualityRank({Codec::Mp3, 320, false, 44100, 0}), 4);
    EXPECT_EQ(qualityRank({Codec::Aac, 256, std::nullopt, 44100, 0}), 4);
    EXPECT_EQ(qualityRank({Codec::Mp3, 245, true, 44100, 0}), 3); // V0
    EXPECT_EQ(qualityRank({Codec::Mp3, 256, false, 44100, 0}), 3);
    EXPECT_EQ(qualityRank({Codec::Mp3, 190, true, 44100, 0}), 2); // V2
    EXPECT_EQ(qualityRank({Codec::Mp3, 192, false, 44100, 0}), 2);
    EXPECT_EQ(qualityRank({Codec::Mp3, 128, false, 44100, 0}), 1);
    EXPECT_EQ(qualityRank({Codec::Mp3, 0, false, 44100, 0}), 0);
    EXPECT_EQ(qualityRank({Codec::Unknown, 320, false, 44100, 0}), 0);
}

TEST(Quality, CodecsFromExtensions)
{
    EXPECT_EQ(selecta::match::codecFromExtension(u"FLAC"_s), Codec::Flac);
    EXPECT_EQ(selecta::match::codecFromExtension(u"m4a"_s), Codec::Aac);
    EXPECT_EQ(selecta::match::codecFromExtension(u"aif"_s), Codec::Aiff);
    EXPECT_EQ(selecta::match::codecFromExtension(u"exe"_s), Codec::Unknown);
    RemoteAttributes alac;
    alac.bitDepth = 16;
    EXPECT_EQ(selecta::match::codecFor(u"m4a"_s, alac), Codec::Alac);
    EXPECT_TRUE(selecta::match::isLossless(Codec::Alac));
    EXPECT_FALSE(selecta::match::isLossless(Codec::Opus));
}

TEST(Quality, EstablishedNeedsPlausibleAttributes) // SRCH-6, SRCH-7, T33
{
    RemoteAttributes claimed;
    claimed.bitrateKbps = 320;
    claimed.durationSec = 300;
    const RemoteQuality real = selecta::match::assessRemoteQuality(u"mp3"_s, 12'100'000, claimed);
    EXPECT_TRUE(real.established);
    EXPECT_TRUE(real.plausible);
    EXPECT_EQ(real.rank, 4);
    EXPECT_EQ(real.estimatedBitrateKbps, 322);

    const RemoteQuality fake = selecta::match::assessRemoteQuality(u"mp3"_s, 4'800'000, claimed); // ≈ 128 kbps
    EXPECT_FALSE(fake.established);
    EXPECT_FALSE(fake.plausible);

    RemoteAttributes noLength = claimed;
    noLength.durationSec = 0;
    const RemoteQuality unchecked = selecta::match::assessRemoteQuality(u"mp3"_s, 12'000'000, noLength);
    EXPECT_FALSE(unchecked.established);
    EXPECT_TRUE(unchecked.plausible);
    EXPECT_EQ(unchecked.rank, 4);

    RemoteAttributes nothing;
    nothing.durationSec = 300;
    const RemoteQuality estimated = selecta::match::assessRemoteQuality(u"mp3"_s, 7'200'000, nothing); // 192 kbps
    EXPECT_FALSE(estimated.established);
    EXPECT_EQ(estimated.rank, 2);

    RemoteAttributes lossless;
    lossless.durationSec = 300;
    lossless.bitDepth = 24;
    EXPECT_TRUE(selecta::match::assessRemoteQuality(u"flac"_s, 60'000'000, lossless).established);
    EXPECT_EQ(selecta::match::assessRemoteQuality(u"flac"_s, 60'000'000, lossless).rank, 6);
    EXPECT_FALSE(selecta::match::assessRemoteQuality(u"flac"_s, 3'000'000, lossless).established); // 80 kbps
    EXPECT_EQ(selecta::match::assessRemoteQuality(u"xyz"_s, 1000, lossless).rank, 0);
}

} // namespace
