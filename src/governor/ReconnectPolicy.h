// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "governor/Decision.h"

#include <QString>

#include <deque>
#include <functional>

namespace selecta::governor {

// When Selecta may (re)connect to the Soulseek server (NET-4, etiquette note §9.1 "Login / reconnect"):
//
// - After an ordinary disconnect: first retry after a random 10–30 s, then doubling with ±25 % jitter, capped at
//   10 min. The delay resets once a session has stayed up for 5 min.
// - After a login timeout (possibly a silent ban): at least 5 min, doubling, capped at 60 min.
// - After an immediate EOF (possibly an IP throttle): 30 min plus 1–5 min.
// - After the server's ban message: 30 min plus 1–5 min, and only once the user reconnects.
// - After `Relogged` or a login rejection: never on its own; only when the user asks. "Server full" waits 5 min.
// - Never more than 10 attempts in any hour.
//
// Not thread-safe; owned by the Governor. `random01` returns uniform numbers in [0, 1) (injected for tests).
class ReconnectPolicy
{
public:
    enum class Event {
        LoginSucceeded,
        ConnectionLost,    // the session dropped after a successful login
        LoginTimeout,      // no reply to the login request
        InstantEof,        // the server closed the connection immediately
        BanNotice,         // the "banned for 30 minutes" message from the server
        Relogged,          // the account logged in elsewhere
        LoginRejected,     // the server refused the login (wrong password, invalid username, …)
        ServerFull,        // the server refused the login because it's full
        ProtocolViolation, // the server sent an oversized or impossible frame (or our codec has a bug)
        UserRequested,     // the user asked to connect
    };

    ReconnectPolicy(const base::Clock &clock, std::function<double()> random01);

    // Whether a login attempt may start now. Refusals: LoginNeedsUser or LoginBackoff (with retryAfter).
    Decision mayLogin() const;
    // Call when a login attempt actually starts.
    void attemptStarted();
    void handle(Event event);

    bool needsUser() const { return m_needsUser; }
    base::Clock::Duration currentDelay() const { return m_delay; }

private:
    base::Clock::Duration jittered(base::Clock::Duration base) const;
    void waitAtLeast(base::Clock::Duration duration);
    void pruneAttempts() const;

    const base::Clock &m_clock;
    std::function<double()> m_random;
    base::Clock::TimePoint m_notBefore;
    base::Clock::Duration m_delay{0};
    std::optional<base::Clock::TimePoint> m_loggedInSince;
    bool m_needsUser = false;
    QString m_needsUserReason;
    mutable std::deque<base::Clock::TimePoint> m_attempts;
};

} // namespace selecta::governor
