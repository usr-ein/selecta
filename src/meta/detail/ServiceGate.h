// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <chrono>
#include <deque>
#include <functional>
#include <optional>

namespace selecta::meta::detail {

// Paces the starts of requests to one service: at least `minSpacing` between starts, at most `windowCount` starts in
// any sliding `window`, at most `maxInFlight` running, none before a back-off ends (blockUntil()).
//
// A ticket goes queued → reserved (its `ready` callback ran: the caller may now ask the governor for a permit) →
// in flight (markStarted(): the request went out) → done (markFinished()). Only one ticket is reserved at a time, and
// spacing is measured between actual starts, so a permit that arrives late can't squeeze two starts together.
// abandon() drops a queued or reserved ticket.
//
// `ready` callbacks always run from the event loop (an internal timer), never inside a call to this class. Times come
// from the injected clock; poll() re-checks now (the timer calls it; tests call it after moving a ManualClock).
//
// Threading: one thread, the owner's. Not thread-safe.
class ServiceGate final : public QObject
{
    Q_OBJECT

public:
    using Ticket = quint64;
    using Ready = std::function<void(Ticket ticket)>;

    struct Limits
    {
        std::chrono::milliseconds minSpacing{0};
        int windowCount = 1;
        std::chrono::milliseconds window{0};
        int maxInFlight = 1;
    };

    ServiceGate(const Limits &limits, const base::Clock &clock, QObject *parent = nullptr);
    ~ServiceGate() override;
    ServiceGate(const ServiceGate &) = delete;
    ServiceGate &operator=(const ServiceGate &) = delete;
    ServiceGate(ServiceGate &&) = delete;
    ServiceGate &operator=(ServiceGate &&) = delete;

    // Queues a start; `front` puts it ahead of the others (a retry keeps its place).
    Ticket enqueue(Ready ready, bool front = false);
    void markStarted(Ticket ticket);
    void markFinished(Ticket ticket);
    void abandon(Ticket ticket);

    // Nothing starts before `until` (a later time extends the back-off, an earlier one is ignored).
    void blockUntil(base::Clock::TimePoint until);
    base::Clock::TimePoint blockedUntil() const { return m_blockedUntil; }
    // How long until the back-off ends; zero when not blocked.
    base::Clock::Duration blockRemaining() const;

    int queued() const { return static_cast<int>(m_queue.size()); }
    int inFlight() const { return static_cast<int>(m_inFlight.size()); }
    bool hasReservation() const { return m_reserved.has_value(); }

    // Releases the next ticket if the limits allow it now; otherwise arms the timer for when they will.
    void poll();

    // poll() on every gate living on the calling thread. For tests that move a ManualClock: the gates' own timers
    // run on real time, so after `clock.advance()` a test calls this instead of waiting.
    static void pollAll();

private:
    struct Waiting
    {
        Ticket ticket = 0;
        Ready ready;
    };

    void schedule(); // poll() on the next event-loop turn
    base::Clock::TimePoint earliestStart(base::Clock::TimePoint now);

    Limits m_limits;
    const base::Clock &m_clock;
    QTimer m_timer;
    std::deque<Waiting> m_queue;
    std::optional<Ticket> m_reserved;
    QSet<Ticket> m_inFlight;
    std::deque<base::Clock::TimePoint> m_starts; // recent starts, oldest first (the sliding window)
    base::Clock::TimePoint m_blockedUntil;
    Ticket m_next = 1;
};

} // namespace selecta::meta::detail
