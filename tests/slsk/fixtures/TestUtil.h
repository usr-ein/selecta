// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "support/PermitFactory.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QTest>
#include <QTimer>

#include <chrono>

// Small helpers for the slsk session tests. Tests run real loopback sockets, so they wait on the event loop (never
// sleep): waitFor() returns as soon as the condition holds.
namespace selecta::slsk::testing {

using namespace std::chrono_literals;

inline constexpr std::chrono::milliseconds kWait = 5s; // generous: only reached when a test fails

// How often waitFor() wakes the event loop when nothing else does, so it sees a condition another thread changes
// without posting an event. Events that do arrive (socket reads, timers, queued signals) wake it at once.
inline constexpr std::chrono::milliseconds kWaitPoll = 5ms;

// Waits until `condition` holds, or `timeout` passes. Main thread (or any thread with an event loop) only.
// It blocks in the event loop until something happens and checks the condition after each batch of events, rather
// than sleeping between passes as QTest::qWaitFor does: that 10 ms sleep let a transfer move on by one socket read per
// pass, so a 15 MB test download took 3.5 s on a Mac and 15 s on a loaded runner (P-030, P-031).
// A condition that already holds returns without spinning the event loop, which could invalidate it. A zero timeout
// still runs one pass. The precise expiry timer alone decides the timeout: a coarse one may fire before a clock-based
// deadline has passed, and the wait always ends at its timeout, never hangs.
template <typename Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = kWait)
{
    if (condition()) {
        return true;
    }
    bool expired = false;
    QTimer expiry;
    expiry.setSingleShot(true);
    expiry.setTimerType(Qt::PreciseTimer);
    QObject::connect(&expiry, &QTimer::timeout, [&expired] { expired = true; });
    expiry.start(timeout);
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.start(kWaitPoll);
    while (!expired) {
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (condition()) {
            return true;
        }
    }
    return false;
}

// Spins the event loop for `duration`, for "nothing else happens" checks.
inline void settle(std::chrono::milliseconds duration = 100ms)
{
    QTest::qWait(duration);
}

inline governor::Permit permit(governor::Operation operation)
{
    return governor::testing::PermitFactory::make(operation);
}

} // namespace selecta::slsk::testing
