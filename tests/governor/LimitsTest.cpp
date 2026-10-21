// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/Limits.h"

#include <gtest/gtest.h>
#include <limits>

namespace limits = selecta::governor::limits;

// --- P-022: the peer socket cap under the process's open-file limit -------------------------------------------------

TEST(SocketCap, NeverExceedsTheCeiling)
{
    for (qint64 openFiles = -10; openFiles <= 20'000; ++openFiles) {
        const int sockets = limits::socketCapFor(openFiles);
        ASSERT_LE(sockets, limits::kSocketsMax) << openFiles;
        ASSERT_GE(sockets, 1) << openFiles;
    }
    // Unlimited (Windows, or an unlimited soft limit) and absurd limits still stop at the ceiling.
    EXPECT_EQ(limits::socketCapFor(std::numeric_limits<int>::max()), limits::kSocketsMax);
    EXPECT_EQ(limits::socketCapFor(std::numeric_limits<qint64>::max()), limits::kSocketsMax);
    EXPECT_EQ(limits::socketCapFor(std::numeric_limits<qint64>::min()), 1);
}

TEST(SocketCap, LeavesTheReserveForTheRestOfTheApp)
{
    for (qint64 openFiles = 1; openFiles <= 20'000; ++openFiles) {
        const qint64 rest = openFiles - limits::socketCapFor(openFiles);
        // The reserve, or three quarters of a limit too low for it.
        ASSERT_GE(rest, std::min<qint64>(limits::kReservedDescriptors, openFiles * 3 / 4)) << openFiles;
    }
    EXPECT_EQ(limits::reservedDescriptors(limits::kWantedOpenFiles), limits::kReservedDescriptors);
}

TEST(SocketCap, GrowsWithTheLimitUpToTheCeiling)
{
    for (qint64 openFiles = 0; openFiles <= 20'000; ++openFiles) {
        ASSERT_LE(limits::socketCapFor(openFiles), limits::socketCapFor(openFiles + 1)) << openFiles;
    }
    const qint64 full = qint64{limits::kSocketsMax} + limits::kReservedDescriptors;
    EXPECT_EQ(limits::socketCapFor(full), limits::kSocketsMax);
    EXPECT_EQ(limits::socketCapFor(full - 1), limits::kSocketsMax - 1);
    EXPECT_EQ(limits::socketCapFor(limits::kWantedOpenFiles), limits::kSocketsMax);
}

TEST(SocketCap, MacOSDefaultLimitLeavesMostDescriptorsToTheRest)
{
    // 256 is what a macOS app gets unless it asks for more: before P-022 the cap (512) was above it.
    EXPECT_EQ(limits::socketCapFor(256), 64);
    EXPECT_EQ(limits::socketCapFor(1), 1);
}
