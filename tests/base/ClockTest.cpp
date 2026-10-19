// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Clock.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::base::SteadyClock;

TEST(Clock, SteadyClockNeverGoesBackwards)
{
    const SteadyClock clock;
    auto previous = clock.now();
    for (int i = 0; i < 1000; ++i) {
        const auto now = clock.now();
        ASSERT_GE(now, previous);
        previous = now;
    }
}

TEST(Clock, ManualClockMovesOnlyWhenAdvanced)
{
    ManualClock clock;
    const auto start = clock.now();
    EXPECT_EQ(clock.now(), start);
    clock.advance(1500ms);
    EXPECT_EQ(clock.now() - start, 1500ms);
}
