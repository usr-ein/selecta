// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ServiceGate.h"

#include "meta/support/Await.h"

#include <QList>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::meta::detail::ServiceGate;
using selecta::meta::testing::spinUntil;

namespace {

struct Harness
{
    explicit Harness(const ServiceGate::Limits &limits)
        : gate(limits, clock)
    {}

    // Queues a ticket whose `ready` records it and, if `start`, starts it at once.
    ServiceGate::Ticket add(bool start = true, bool front = false,
                            selecta::meta::Priority priority = selecta::meta::Priority::Interactive)
    {
        return gate.enqueue(
            [this, start](ServiceGate::Ticket ticket) {
                released.append(ticket);
                if (start) {
                    gate.markStarted(ticket);
                }
            },
            priority, front);
    }

    // Lets queued zero-delay work run.
    void settle() { QTest::qWait(5); }

    ManualClock clock;
    ServiceGate gate;
    QList<ServiceGate::Ticket> released;
};

} // namespace

TEST(ServiceGate, ReleasesFromTheEventLoopNeverInsideEnqueue)
{
    Harness h({0ms, 10, 1s, 10});
    h.add();
    EXPECT_TRUE(h.released.isEmpty());
    ASSERT_TRUE(spinUntil([&] { return h.released.size() == 1; }));
}

TEST(ServiceGate, SpacesStartsByTheMinimumSpacing)
{
    Harness h({1000ms, 100, 1s, 10});
    h.add();
    h.add();
    h.settle();
    ASSERT_EQ(h.released.size(), 1);
    h.clock.advance(999ms);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 1);
    h.clock.advance(1ms);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 2);
}

TEST(ServiceGate, SlidingWindowCapsStarts)
{
    Harness h({0ms, 3, 10s, 10}); // ≤ 3 starts in any 10 s
    for (int i = 0; i < 5; ++i) {
        h.add();
    }
    ASSERT_TRUE(spinUntil([&] { return h.released.size() == 3; }));
    h.settle();
    EXPECT_EQ(h.released.size(), 3);
    h.clock.advance(9999ms);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 3);
    h.clock.advance(1ms);
    h.gate.poll();
    h.settle();
    EXPECT_EQ(h.released.size(), 5); // the first three left the window together
}

TEST(ServiceGate, InFlightLimitWaitsForFinish)
{
    Harness h({0ms, 100, 1s, 1});
    const auto first = h.add();
    h.add();
    h.settle();
    ASSERT_EQ(h.released.size(), 1);
    h.clock.advance(10s);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 1);
    h.gate.markFinished(first);
    ASSERT_TRUE(spinUntil([&] { return h.released.size() == 2; }));
}

TEST(ServiceGate, OnlyOneReservationAtATime)
{
    Harness h({0ms, 100, 1s, 10});
    const auto first = h.add(false); // released but not started
    h.add();
    h.settle();
    ASSERT_EQ(h.released.size(), 1);
    EXPECT_TRUE(h.gate.hasReservation());
    h.gate.markStarted(first);
    ASSERT_TRUE(spinUntil([&] { return h.released.size() == 2; }));
}

TEST(ServiceGate, SpacingCountsFromTheActualStart)
{
    Harness h({1000ms, 100, 1s, 10});
    const auto first = h.add(false);
    h.add();
    h.settle();
    h.clock.advance(5s); // the permit for the first took a while
    h.gate.markStarted(first);
    h.settle();
    EXPECT_EQ(h.released.size(), 1); // the second still waits a full spacing after the real start
    h.clock.advance(1000ms);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 2);
}

TEST(ServiceGate, AbandoningAReservationReleasesTheNext)
{
    Harness h({0ms, 100, 1s, 10});
    const auto first = h.add(false);
    h.add();
    h.settle();
    h.gate.abandon(first);
    ASSERT_TRUE(spinUntil([&] { return h.released.size() == 2; }));
}

TEST(ServiceGate, AbandoningAQueuedTicketSkipsIt)
{
    Harness h({1000ms, 100, 1s, 10});
    h.add();
    const auto second = h.add();
    const auto third = h.add();
    h.settle();
    h.gate.abandon(second);
    h.clock.advance(1000ms);
    h.gate.poll();
    ASSERT_EQ(h.released.size(), 2);
    EXPECT_EQ(h.released.at(1), third);
    EXPECT_EQ(h.gate.queued(), 0);
}

TEST(ServiceGate, FrontGoesFirst)
{
    Harness h({1000ms, 100, 1s, 10});
    h.add();
    h.settle(); // the first one is out
    h.add();
    const auto urgent = h.add(true, true);
    h.settle();
    h.clock.advance(1000ms);
    h.gate.poll();
    ASSERT_EQ(h.released.size(), 2);
    EXPECT_EQ(h.released.at(1), urgent);
}

TEST(ServiceGate, InteractiveJumpsAheadOfABackgroundBatch)
{
    using selecta::meta::Priority;
    Harness h({1000ms, 100, 1s, 10});
    h.add();
    h.settle(); // one is out; the rest must wait for the spacing
    QList<ServiceGate::Ticket> batch;
    for (int i = 0; i < 50; ++i) {
        batch.append(h.add(true, false, Priority::Background));
    }
    const auto preview = h.add(true, false, Priority::Interactive);
    const auto retry = h.add(true, true, Priority::Background); // front of its own lane only
    h.settle();
    h.clock.advance(1000ms);
    h.gate.poll();
    ASSERT_EQ(h.released.size(), 2);
    EXPECT_EQ(h.released.at(1), preview);
    h.clock.advance(1000ms);
    h.gate.poll();
    ASSERT_EQ(h.released.size(), 3);
    EXPECT_EQ(h.released.at(2), retry);
    h.clock.advance(1000ms);
    h.gate.poll();
    EXPECT_EQ(h.released.at(3), batch.first());
    EXPECT_EQ(h.gate.queued(), 49);
}

TEST(ServiceGate, AbandonFindsTicketsInEitherLane)
{
    using selecta::meta::Priority;
    Harness h({1000ms, 100, 1s, 10});
    h.add();
    h.settle();
    const auto background = h.add(true, false, Priority::Background);
    const auto interactive = h.add(true, false, Priority::Interactive);
    h.gate.abandon(background);
    h.gate.abandon(interactive);
    EXPECT_EQ(h.gate.queued(), 0);
}

TEST(ServiceGate, BlockHoldsEverythingUntilItEnds)
{
    Harness h({0ms, 100, 1s, 10});
    h.gate.blockUntil(h.clock.now() + 30s);
    EXPECT_EQ(h.gate.blockRemaining(), 30s);
    h.add();
    h.settle();
    EXPECT_TRUE(h.released.isEmpty());
    h.gate.blockUntil(h.clock.now() + 10s); // an earlier end doesn't shorten the block
    h.clock.advance(29s);
    h.gate.poll();
    EXPECT_TRUE(h.released.isEmpty());
    h.clock.advance(1s);
    h.gate.poll();
    EXPECT_EQ(h.released.size(), 1);
    EXPECT_EQ(h.gate.blockRemaining(), 0ms);
}

TEST(ServiceGate, TimerReleasesWithRealTimeToo)
{
    const selecta::base::SteadyClock clock;
    ServiceGate gate({50ms, 100, 1s, 10}, clock);
    int released = 0;
    for (int i = 0; i < 3; ++i) {
        gate.enqueue([&](ServiceGate::Ticket ticket) {
            ++released;
            gate.markStarted(ticket);
        });
    }
    EXPECT_TRUE(spinUntil([&] { return released == 3; }, 2000));
}

TEST(ServiceGate, PollAllReachesEveryGateOfTheThread)
{
    Harness a({1000ms, 100, 1s, 10});
    Harness b({1000ms, 100, 1s, 10});
    a.add();
    a.add();
    b.add();
    b.add();
    a.settle();
    a.clock.advance(1s);
    b.clock.advance(1s);
    ServiceGate::pollAll();
    EXPECT_EQ(a.released.size(), 2);
    EXPECT_EQ(b.released.size(), 2);
}
