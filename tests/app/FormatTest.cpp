// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Format.h"

#include "match/Types.h"

#include <gtest/gtest.h>
#include <limits>

using namespace Qt::StringLiterals;
namespace format = selecta::app::format;

TEST(FormatTest, ClockShowsMinutesAndSeconds)
{
    EXPECT_EQ(format::clock(0.0), u"0:00"_s);
    EXPECT_EQ(format::clock(5.99), u"0:05"_s); // a playhead never reads ahead
    EXPECT_EQ(format::clock(83.4), u"1:23"_s);
    EXPECT_EQ(format::clock(3725.0), u"62:05"_s);
}

TEST(FormatTest, ClockWithTenths)
{
    EXPECT_EQ(format::clock(83.46, 1), u"1:23.4"_s);
    EXPECT_EQ(format::clock(0.05, 1), u"0:00.0"_s);
    EXPECT_EQ(format::clock(59.99, 1), u"0:59.9"_s);
}

TEST(FormatTest, ClockTreatsNonsenseAsZero)
{
    EXPECT_EQ(format::clock(-3.0), u"0:00"_s);
    EXPECT_EQ(format::clock(std::numeric_limits<double>::quiet_NaN()), u"0:00"_s);
    EXPECT_EQ(format::clock(std::numeric_limits<double>::infinity(), 1), u"0:00.0"_s);
}

TEST(FormatTest, LengthRoundsToTheSecond)
{
    EXPECT_EQ(format::length(372'400), u"6:12"_s);
    EXPECT_EQ(format::length(3'723'000), u"1:02:03"_s);
    EXPECT_TRUE(format::length(std::nullopt).isEmpty());
    EXPECT_TRUE(format::length(0).isEmpty());
}

TEST(FormatTest, AboutRoundsUpToTheMinute)
{
    EXPECT_EQ(selecta::app::format::about(0), u"under a minute"_s);
    EXPECT_EQ(selecta::app::format::about(59'999), u"under a minute"_s);
    EXPECT_EQ(selecta::app::format::about(60'000), u"about 1 min"_s);
    EXPECT_EQ(selecta::app::format::about(61'000), u"about 2 min"_s);
    EXPECT_EQ(selecta::app::format::about(3'600'000), u"about 1 h"_s);
    EXPECT_EQ(selecta::app::format::about(4'800'000), u"about 1 h 20 min"_s);
}

// P-010: the collection's files, as the Collection view words their quality.
TEST(FormatTest, ALocalFilesQuality)
{
    using format::quality;
    using selecta::match::AudioQuality;
    using selecta::match::Codec;
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Mp3, .bitrateKbps = 320, .vbr = false}), u"MP3 320"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Mp3, .bitrateKbps = 245, .vbr = true}), u"MP3 VBR 245"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Mp3}), u"MP3"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Aac, .bitrateKbps = 256}), u"AAC 256"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Flac, .sampleRateHz = 44100, .bitDepth = 16}), u"FLAC 16/44.1"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Wav, .sampleRateHz = 96000, .bitDepth = 24}), u"WAV 24/96"_s);
    EXPECT_EQ(quality(AudioQuality{.codec = Codec::Alac}), u"ALAC"_s);
    EXPECT_EQ(quality(AudioQuality{}), u"?"_s);
}
