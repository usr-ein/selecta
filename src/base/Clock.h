// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>

namespace selecta::base {

// Monotonic time. Policy code (the governor, retries, timeouts) takes a Clock so tests can drive time instead of
// sleeping (ADR-006).
class Clock
{
public:
    using Duration = std::chrono::milliseconds;
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock, Duration>;

    Clock() = default;
    Clock(const Clock &) = delete;
    Clock &operator=(const Clock &) = delete;
    virtual ~Clock();

    virtual TimePoint now() const = 0;
};

// The real monotonic clock.
class SteadyClock final : public Clock
{
public:
    TimePoint now() const override;
};

// A clock that moves only when told to. For tests.
class ManualClock final : public Clock
{
public:
    explicit ManualClock(TimePoint start = TimePoint{})
        : m_now(start)
    {}

    TimePoint now() const override { return m_now; }
    void advance(Duration by) { m_now += by; }

private:
    TimePoint m_now;
};

} // namespace selecta::base
