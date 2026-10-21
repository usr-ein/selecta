// SPDX-License-Identifier: GPL-3.0-or-later
// The shared test wait, waitFor(): it returns as soon as its condition holds, blocks in the event loop rather than
// sleeping between passes, sees a condition another thread changes, and ends at its timeout when nothing happens.
#include "fixtures/TestUtil.h"

#include <QElapsedTimer>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

namespace selecta::slsk::testing {
namespace {

using namespace std::chrono_literals;

TEST(TestUtilTest, AConditionThatAlreadyHoldsReturnsWithoutSpinningTheEventLoop)
{
    bool ran = false;
    QTimer::singleShot(0, [&ran] { ran = true; });
    EXPECT_TRUE(waitFor([] { return true; }));
    EXPECT_FALSE(ran); // spinning could deliver events that invalidate the condition
    EXPECT_TRUE(waitFor([&ran] { return ran; }));
}

// Each zero-time timer is only delivered on the event loop's next pass. QTest::qWaitFor sleeps 10 ms after each pass,
// so this chain took over 4 s; blocking in the event loop runs it in milliseconds.
TEST(TestUtilTest, AChainOfEventsRunsWithoutASleepBetweenPasses)
{
    constexpr int kSteps = 400;
    int steps = 0;
    std::function<void()> step = [&] {
        if (++steps < kSteps) {
            QTimer::singleShot(0, step);
        }
    };
    QTimer::singleShot(0, step);
    QElapsedTimer elapsed;
    elapsed.start();
    EXPECT_TRUE(waitFor([&steps] { return steps == kSteps; }));
    EXPECT_LT(elapsed.elapsed(), 2000);
}

// Nothing wakes the event loop when another thread changes the condition without posting an event: the wait still
// sees it, well before its timeout.
TEST(TestUtilTest, SeesAConditionAnotherThreadChanges)
{
    std::atomic<bool> done = false;
    std::thread worker([&done] {
        std::this_thread::sleep_for(50ms);
        done = true;
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const bool seen = waitFor([&done] { return done.load(); });
    worker.join();
    EXPECT_TRUE(seen);
    EXPECT_LT(elapsed.elapsed(), 2000);
}

// A failing wait ends at its timeout, never hangs, with nothing at all going on. 1 s takes Qt's coarse-timer path,
// which may fire early; the timeout is never cut short.
TEST(TestUtilTest, AFailingWaitEndsAtItsTimeout)
{
    QElapsedTimer elapsed;
    elapsed.start();
    EXPECT_FALSE(waitFor([] { return false; }, 1s));
    EXPECT_GE(elapsed.elapsed(), 1000);
    EXPECT_LT(elapsed.elapsed(), 3000);

    elapsed.restart();
    EXPECT_FALSE(waitFor([] { return false; }, 50ms));
    EXPECT_GE(elapsed.elapsed(), 50);
    EXPECT_LT(elapsed.elapsed(), 2000);
}

// A zero timeout still gives the event loop one pass, as QTest::qWaitFor does.
TEST(TestUtilTest, AZeroTimeoutStillRunsOnePass)
{
    bool ran = false;
    QTimer::singleShot(0, [&ran] { ran = true; });
    EXPECT_TRUE(waitFor([&ran] { return ran; }, 0ms));
}

// The wait ends at its timeout even while events keep arriving that never satisfy the condition.
TEST(TestUtilTest, AFailingWaitEndsAtItsTimeoutUnderABusyEventLoop)
{
    QTimer busy;
    busy.start(0);
    int ticks = 0;
    QObject::connect(&busy, &QTimer::timeout, [&ticks] { ++ticks; });
    QElapsedTimer elapsed;
    elapsed.start();
    EXPECT_FALSE(waitFor([] { return false; }, 200ms));
    EXPECT_GE(elapsed.elapsed(), 200);
    EXPECT_LT(elapsed.elapsed(), 2000);
    EXPECT_GT(ticks, 0);
}

// Objects deleted later are deleted between passes, as with QTest::qWaitFor: tests wait for a deleteLater().
TEST(TestUtilTest, DeferredDeletesRunDuringTheWait)
{
    auto *object = new QObject;
    bool destroyed = false;
    QObject::connect(object, &QObject::destroyed, [&destroyed] { destroyed = true; });
    object->deleteLater();
    EXPECT_TRUE(waitFor([&destroyed] { return destroyed; }));
}

} // namespace
} // namespace selecta::slsk::testing
