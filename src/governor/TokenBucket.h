// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"

namespace selecta::governor {

// A token bucket with whole tokens: at most `capacity`, one more every `refillInterval`. The governor's rates are
// buckets or sliding windows, never fixed windows, so no span of time can exceed its budget (ADR-006).
//
// Not thread-safe: the governor owns its buckets on one thread. The const members don't modify anything, so they may
// be called concurrently with each other, but not with tryTake().
class TokenBucket
{
public:
    // Starts full unless `initialTokens` says otherwise (clamped to 0…capacity). Capacity and interval must be
    // positive; in release builds they are clamped to 1 and 1 ms.
    TokenBucket(const base::Clock &clock, int capacity, base::Clock::Duration refillInterval, int initialTokens = -1);

    int capacity() const { return m_capacity; }
    base::Clock::Duration refillInterval() const { return m_interval; }

    // Whole tokens available now.
    int available() const;

    // Takes `count` tokens if they are all available now; otherwise takes nothing. A non-positive count succeeds and
    // takes nothing.
    bool tryTake(int count = 1);

    // How long until `count` tokens are available: zero if they already are. A count above the capacity is treated
    // as the capacity.
    base::Clock::Duration timeUntilAvailable(int count = 1) const;

private:
    struct Level
    {
        int tokens = 0;
        base::Clock::TimePoint since; // when the token being earned started accruing (default: epoch)
    };
    Level levelAt(base::Clock::TimePoint now) const;

    const base::Clock &m_clock;
    int m_capacity;
    base::Clock::Duration m_interval;
    Level m_level;
};

} // namespace selecta::governor
