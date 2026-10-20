// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/detail/ServiceGate.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFuture>
#include <QTest>

#include <chrono>
#include <functional>
#include <optional>

namespace selecta::meta::testing {

inline constexpr int kAwaitMs = 5000;

// Runs the event loop until `done()` or the deadline.
inline bool spinUntil(const std::function<bool()> &done, int timeoutMs = kAwaitMs)
{
    return QTest::qWaitFor(done, timeoutMs);
}

// Runs the event loop until the future finishes and returns its result; nullopt if it didn't finish in time or ended
// canceled.
template <typename T>
std::optional<T> await(const QFuture<T> &future, int timeoutMs = kAwaitMs)
{
    if (!spinUntil([&] { return future.isFinished(); }, timeoutMs)) {
        return std::nullopt;
    }
    if (future.isCanceled() || future.resultCount() == 0) {
        return std::nullopt;
    }
    return future.result();
}

// Like await(), but moves a ManualClock forward by `step` whenever nothing else happens, so paced requests (which
// wait on the clock) make progress without real waiting.
template <typename T>
std::optional<T> awaitAdvancing(const QFuture<T> &future, base::ManualClock &clock,
                                std::chrono::milliseconds step = std::chrono::milliseconds(1000),
                                int timeoutMs = kAwaitMs)
{
    const QDeadlineTimer deadline(timeoutMs);
    while (!future.isFinished() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(1);
        if (!future.isFinished()) {
            clock.advance(step);
            detail::ServiceGate::pollAll();
        }
    }
    if (!future.isFinished() || future.isCanceled() || future.resultCount() == 0) {
        return std::nullopt;
    }
    return future.result();
}

// True when the future is still running after a few turns of the event loop.
template <typename T>
bool staysPending(const QFuture<T> &future, int ms = 50)
{
    QTest::qWait(ms);
    return !future.isFinished();
}

} // namespace selecta::meta::testing
