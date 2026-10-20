// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ServiceGate.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace selecta::meta::detail {

namespace {

// The gates of this thread, for pollAll().
std::vector<ServiceGate *> &gatesOfThisThread()
{
    thread_local std::vector<ServiceGate *> gates;
    return gates;
}

} // namespace

ServiceGate::ServiceGate(const Limits &limits, const base::Clock &clock, QObject *parent)
    : QObject(parent)
    , m_limits(limits)
    , m_clock(clock)
{
    m_limits.windowCount = std::max(1, m_limits.windowCount);
    m_limits.maxInFlight = std::max(1, m_limits.maxInFlight);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer); // spacing must never come out shorter than asked
    connect(&m_timer, &QTimer::timeout, this, &ServiceGate::poll);
    gatesOfThisThread().push_back(this);
}

ServiceGate::~ServiceGate()
{
    std::erase(gatesOfThisThread(), this);
}

void ServiceGate::pollAll()
{
    const std::vector<ServiceGate *> gates = gatesOfThisThread(); // poll() may create or destroy gates
    for (ServiceGate *gate : gates) {
        if (std::ranges::find(gatesOfThisThread(), gate) != gatesOfThisThread().end()) {
            gate->poll();
        }
    }
}

ServiceGate::Ticket ServiceGate::enqueue(Ready ready, Priority priority, bool front)
{
    const Ticket ticket = m_next++;
    std::deque<Waiting> &lane = priority == Priority::Interactive ? m_interactive : m_background;
    if (front) {
        lane.push_front({ticket, std::move(ready)});
    } else {
        lane.push_back({ticket, std::move(ready)});
    }
    schedule();
    return ticket;
}

void ServiceGate::markStarted(Ticket ticket)
{
    if (m_reserved != ticket) {
        return;
    }
    m_reserved.reset();
    m_inFlight.insert(ticket);
    m_starts.push_back(m_clock.now());
    while (std::cmp_greater(m_starts.size(), m_limits.windowCount)) {
        m_starts.pop_front();
    }
    schedule();
}

void ServiceGate::markFinished(Ticket ticket)
{
    if (m_inFlight.remove(ticket)) {
        schedule();
    }
}

void ServiceGate::abandon(Ticket ticket)
{
    if (m_reserved == ticket) {
        m_reserved.reset();
        schedule();
        return;
    }
    for (std::deque<Waiting> *lane : {&m_interactive, &m_background}) {
        const auto it = std::ranges::find(*lane, ticket, &Waiting::ticket);
        if (it != lane->end()) {
            lane->erase(it);
            return;
        }
    }
}

void ServiceGate::blockUntil(base::Clock::TimePoint until)
{
    if (until > m_blockedUntil) {
        m_blockedUntil = until;
        schedule();
    }
}

base::Clock::Duration ServiceGate::blockRemaining() const
{
    const auto now = m_clock.now();
    return m_blockedUntil > now ? m_blockedUntil - now : base::Clock::Duration::zero();
}

void ServiceGate::schedule()
{
    m_timer.start(0);
}

base::Clock::Duration ServiceGate::nextStartIn() const
{
    const auto now = m_clock.now();
    return earliestStart(now) - now;
}

base::Clock::TimePoint ServiceGate::earliestStart(base::Clock::TimePoint now) const
{
    base::Clock::TimePoint earliest = std::max(now, m_blockedUntil);
    if (!m_starts.empty()) {
        earliest = std::max(earliest, m_starts.back() + m_limits.minSpacing);
    }
    if (std::cmp_greater_equal(m_starts.size(), m_limits.windowCount)) {
        // The oldest start inside the window must leave it first.
        earliest = std::max(earliest, m_starts.front() + m_limits.window);
    }
    return earliest;
}

void ServiceGate::poll()
{
    if (m_reserved.has_value() || (m_interactive.empty() && m_background.empty()) ||
        static_cast<int>(m_inFlight.size()) >= m_limits.maxInFlight) {
        return; // markStarted / abandon / markFinished / enqueue schedule the next look
    }
    const base::Clock::TimePoint now = m_clock.now();
    const base::Clock::TimePoint earliest = earliestStart(now);
    if (earliest > now) {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
        m_timer.start(std::max<std::chrono::milliseconds>(wait, std::chrono::milliseconds(1)));
        return;
    }
    std::deque<Waiting> &lane = m_interactive.empty() ? m_background : m_interactive;
    Waiting next = std::move(lane.front());
    lane.pop_front();
    m_reserved = next.ticket;
    next.ready(next.ticket);
}

} // namespace selecta::meta::detail
