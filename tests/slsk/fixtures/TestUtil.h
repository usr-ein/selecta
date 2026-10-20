// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "support/PermitFactory.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QTest>

#include <chrono>

// Small helpers for the slsk session tests. Tests run real loopback sockets, so they wait on the event loop (never
// sleep): waitFor() returns as soon as the condition holds.
namespace selecta::slsk::testing {

using namespace std::chrono_literals;

inline constexpr std::chrono::milliseconds kWait = 5s; // generous: only reached when a test fails

template <typename Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = kWait)
{
    return QTest::qWaitFor(condition, QDeadlineTimer(timeout));
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
