// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/TokenBucket.h"

#include "base/Clock.h"
#include "governor/Limits.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::governor::TokenBucket;

TEST(TokenBucket, StartsFullAndEmpties)
{
    ManualClock clock;
    TokenBucket bucket(clock, 3, 10s);
    EXPECT_EQ(bucket.available(), 3);
    EXPECT_TRUE(bucket.tryTake());
    EXPECT_TRUE(bucket.tryTake(2));
    EXPECT_EQ(bucket.available(), 0);
    EXPECT_FALSE(bucket.tryTake());
}

TEST(TokenBucket, RefillsOneTokenPerInterval)
{
    ManualClock clock;
    TokenBucket bucket(clock, 3, 10s, 0);
    clock.advance(9999ms);
    EXPECT_EQ(bucket.available(), 0);
    clock.advance(1ms);
    EXPECT_EQ(bucket.available(), 1);
    clock.advance(25s);
    EXPECT_EQ(bucket.available(), 3);
}

TEST(TokenBucket, PartialProgressSurvivesATake)
{
    ManualClock clock;
    TokenBucket bucket(clock, 2, 10s, 0);
    clock.advance(15s); // one token earned, 5 s towards the next
    ASSERT_TRUE(bucket.tryTake());
    EXPECT_EQ(bucket.timeUntilAvailable(), 5s);
    clock.advance(5s);
    EXPECT_EQ(bucket.available(), 1);
}

TEST(TokenBucket, FullBucketDoesNotBankTime)
{
    ManualClock clock;
    TokenBucket bucket(clock, 2, 10s);
    clock.advance(1h);
    ASSERT_TRUE(bucket.tryTake(2));
    EXPECT_EQ(bucket.available(), 0);
    EXPECT_EQ(bucket.timeUntilAvailable(), 10s); // a full interval, not credit from the idle hour
}

TEST(TokenBucket, TimeUntilAvailableForSeveralTokens)
{
    ManualClock clock;
    TokenBucket bucket(clock, 6, 30s, 0);
    EXPECT_EQ(bucket.timeUntilAvailable(1), 30s);
    EXPECT_EQ(bucket.timeUntilAvailable(3), 90s);
    EXPECT_EQ(bucket.timeUntilAvailable(99), 180s); // capped at capacity
    EXPECT_EQ(bucket.timeUntilAvailable(0), 0s);
}

TEST(TokenBucket, NonPositiveTakesSucceedAndTakeNothing)
{
    ManualClock clock;
    TokenBucket bucket(clock, 2, 1s);
    EXPECT_TRUE(bucket.tryTake(0));
    EXPECT_TRUE(bucket.tryTake(-5));
    EXPECT_EQ(bucket.available(), 2);
}

TEST(TokenBucket, FailedTakeTakesNothing)
{
    ManualClock clock;
    TokenBucket bucket(clock, 3, 1s, 2);
    EXPECT_FALSE(bucket.tryTake(3));
    EXPECT_EQ(bucket.available(), 2);
}

TEST(TokenBucket, InitialTokensAreClamped)
{
    ManualClock clock;
    EXPECT_EQ(TokenBucket(clock, 3, 1s, 10).available(), 3);
    EXPECT_EQ(TokenBucket(clock, 3, 1s, 0).available(), 0);
}

TEST(TokenBucket, ConstQueriesDoNotChangeState)
{
    ManualClock clock;
    TokenBucket bucket(clock, 2, 10s, 0);
    clock.advance(7s);
    const TokenBucket &view = bucket;
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(view.available(), 0);
        EXPECT_EQ(view.timeUntilAvailable(), 3s);
    }
}

// D31: a burst of 6, then one every 30 s, means at most 26 searches in any 10 minutes and 13 in any 220 s.
TEST(TokenBucket, SearchBudgetMatchesTheEtiquetteNumbers)
{
    namespace limits = selecta::governor::limits;
    ManualClock clock;
    TokenBucket bucket(clock, limits::kSearchBurst, limits::kSearchRefill);

    const auto countIn = [&](std::chrono::seconds window) {
        int sent = 0;
        for (auto elapsed = 0s; elapsed < window; elapsed += 1s) {
            while (bucket.tryTake())
                ++sent;
            clock.advance(1s);
        }
        return sent;
    };
    EXPECT_EQ(countIn(10min), 6 + 19); // 6 at once, then one at 30 s, 60 s … 570 s
    clock.advance(1h);
    EXPECT_LE(countIn(220s), 13);
}
