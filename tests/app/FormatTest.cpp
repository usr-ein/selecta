// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Format.h"

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
