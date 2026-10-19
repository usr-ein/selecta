// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/TokenBucket.h"

#include <QtGlobal>

#include <algorithm>

namespace selecta::governor {

TokenBucket::TokenBucket(const base::Clock &clock, int capacity, base::Clock::Duration refillInterval,
                         int initialTokens)
    : m_clock(clock)
    , m_capacity(std::max(1, capacity))
    , m_interval(std::max(base::Clock::Duration{1}, refillInterval))
    , m_level{initialTokens < 0 ? m_capacity : std::min(initialTokens, m_capacity), clock.now()}
{
    Q_ASSERT_X(capacity > 0, "TokenBucket", "capacity must be positive");
    Q_ASSERT_X(refillInterval > base::Clock::Duration::zero(), "TokenBucket", "refill interval must be positive");
}

TokenBucket::Level TokenBucket::levelAt(base::Clock::TimePoint now) const
{
    Level level = m_level;
    if (level.tokens >= m_capacity)
        return {m_capacity, now}; // a full bucket doesn't bank time towards the next token
    if (now <= level.since)
        return level; // no time has passed (or the clock is behind): nothing earned
    const auto earned = (now - level.since) / m_interval;
    if (earned >= m_capacity - level.tokens)
        return {m_capacity, now};
    level.tokens += static_cast<int>(earned);
    level.since += earned * m_interval;
    return level;
}

int TokenBucket::available() const
{
    return levelAt(m_clock.now()).tokens;
}

bool TokenBucket::tryTake(int count)
{
    if (count <= 0)
        return true;
    Level level = levelAt(m_clock.now());
    if (count > level.tokens)
        return false;
    level.tokens -= count;
    m_level = level;
    return true;
}

base::Clock::Duration TokenBucket::timeUntilAvailable(int count) const
{
    const base::Clock::TimePoint now = m_clock.now();
    const Level level = levelAt(now);
    const int wanted = std::min(count, m_capacity);
    if (wanted <= level.tokens)
        return base::Clock::Duration::zero();
    const base::Clock::TimePoint ready = level.since + (wanted - level.tokens) * m_interval;
    return ready > now ? ready - now : base::Clock::Duration::zero();
}

} // namespace selecta::governor
