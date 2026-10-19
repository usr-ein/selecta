// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/ReconnectPolicy.h"

#include "base/Clock.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::governor::ReconnectPolicy;
using selecta::governor::Refusal;
using Event = ReconnectPolicy::Event;

namespace {

class ReconnectPolicyTest : public ::testing::Test
{
protected:
    ManualClock clock;
    double random = 0.5; // the middle of the jitter range: ×1.0
    ReconnectPolicy policy{clock, [this] { return random; }};

    bool canLogin() const { return policy.mayLogin().refusal == Refusal::None; }
    std::chrono::milliseconds waitLeft() const { return policy.mayLogin().retryAfter; }
};

} // namespace

TEST_F(ReconnectPolicyTest, FirstLoginIsAllowed)
{
    EXPECT_TRUE(canLogin());
}

TEST_F(ReconnectPolicyTest, FirstRetryIsBetween10And30Seconds)
{
    random = 0.0;
    policy.handle(Event::ConnectionLost);
    EXPECT_EQ(waitLeft(), 10s);

    ReconnectPolicy other(clock, [] { return 1.0; });
    other.handle(Event::ConnectionLost);
    EXPECT_EQ(other.mayLogin().retryAfter, 30s);
}

TEST_F(ReconnectPolicyTest, RetriesDoubleUpToTenMinutes)
{
    random = 0.0;
    policy.handle(Event::ConnectionLost); // 10 s
    random = 0.5;
    std::chrono::milliseconds previous = policy.currentDelay();
    for (int i = 0; i < 12; ++i) {
        clock.advance(policy.currentDelay());
        policy.handle(Event::ConnectionLost);
        EXPECT_GE(policy.currentDelay(), std::min<std::chrono::milliseconds>(previous * 2, 10min));
        EXPECT_LE(policy.currentDelay(), std::chrono::milliseconds(10min));
        previous = policy.currentDelay();
    }
    EXPECT_EQ(policy.currentDelay(), std::chrono::milliseconds(10min));
}

TEST_F(ReconnectPolicyTest, StableSessionResetsTheDelay)
{
    random = 0.0;
    for (int i = 0; i < 6; ++i) {
        clock.advance(policy.currentDelay());
        policy.handle(Event::ConnectionLost);
    }
    ASSERT_GT(policy.currentDelay(), std::chrono::milliseconds(60s));
    clock.advance(policy.currentDelay());
    policy.handle(Event::LoginSucceeded);
    clock.advance(5min);
    policy.handle(Event::ConnectionLost);
    EXPECT_EQ(policy.currentDelay(), std::chrono::milliseconds(10s));
}

TEST_F(ReconnectPolicyTest, LoginTimeoutWaitsAtLeastFiveMinutesThenDoubles)
{
    policy.handle(Event::LoginTimeout);
    EXPECT_EQ(waitLeft(), 5min);
    clock.advance(5min);
    policy.handle(Event::LoginTimeout);
    EXPECT_EQ(waitLeft(), 10min);
    for (int i = 0; i < 6; ++i) {
        clock.advance(policy.currentDelay());
        policy.handle(Event::LoginTimeout);
    }
    EXPECT_EQ(policy.currentDelay(), std::chrono::milliseconds(60min));
}

TEST_F(ReconnectPolicyTest, BanNeedsTheUserAndWaitsThirtyOneToThirtyFiveMinutes)
{
    random = 0.0;
    policy.handle(Event::BanNotice);
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginNeedsUser);
    EXPECT_FALSE(policy.mayLogin().explanation.isEmpty());

    policy.handle(Event::UserRequested); // the user can't skip the wait
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginBackoff);
    EXPECT_EQ(waitLeft(), 31min);
    clock.advance(31min);
    EXPECT_TRUE(canLogin());
}

TEST_F(ReconnectPolicyTest, ReloggedAndRejectionNeverReconnectAlone)
{
    policy.handle(Event::Relogged);
    clock.advance(24h);
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginNeedsUser);
    policy.handle(Event::UserRequested);
    EXPECT_TRUE(canLogin());

    policy.handle(Event::LoginRejected);
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginNeedsUser);
}

TEST_F(ReconnectPolicyTest, ProtocolViolationNeedsTheUser)
{
    policy.handle(Event::ProtocolViolation);
    clock.advance(24h);
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginNeedsUser);
    EXPECT_FALSE(policy.mayLogin().explanation.isEmpty());
    policy.handle(Event::UserRequested);
    EXPECT_TRUE(canLogin());
}

TEST_F(ReconnectPolicyTest, InstantEofWaitsHalfAnHour)
{
    random = 1.0;
    policy.handle(Event::InstantEof);
    EXPECT_EQ(waitLeft(), 35min);
}

TEST_F(ReconnectPolicyTest, ServerFullWaitsFiveMinutesWithoutNeedingTheUser)
{
    policy.handle(Event::ServerFull);
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginBackoff);
    EXPECT_EQ(waitLeft(), 5min);
}

TEST_F(ReconnectPolicyTest, AtMostTenAttemptsPerHour)
{
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(canLogin()) << i;
        policy.attemptStarted();
        clock.advance(1min);
    }
    EXPECT_EQ(policy.mayLogin().refusal, Refusal::LoginBackoff);
    EXPECT_EQ(waitLeft(), 50min); // the first attempt leaves the window 60 min after it started
    clock.advance(50min);
    EXPECT_TRUE(canLogin());
}
