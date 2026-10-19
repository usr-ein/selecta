// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/ReconnectPolicy.h"

#include "governor/Limits.h"

#include <algorithm>
#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::governor {
namespace {

constexpr auto kStableSession = 5min;
constexpr auto kInstantEofWait = 30min;
constexpr auto kServerFullWait = 5min;
constexpr auto kAttemptWindow = 1h;
constexpr auto kBanJitterMin = 1min;
constexpr auto kBanJitterSpan = 4min;

using Duration = base::Clock::Duration;

Duration scaled(Duration duration, double factor)
{
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(duration) * factor);
}

QString describe(Duration wait)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(wait).count();
    if (seconds < 120) {
        return u"%1 s"_s.arg(std::max<qint64>(1, seconds));
    }
    return u"%1 min"_s.arg((seconds + 59) / 60);
}

} // namespace

ReconnectPolicy::ReconnectPolicy(const base::Clock &clock, std::function<double()> random01)
    : m_clock(clock)
    , m_random(std::move(random01))
    , m_notBefore(clock.now())
{}

Duration ReconnectPolicy::jittered(Duration base) const
{
    const double r = std::clamp(m_random(), 0.0, 1.0);
    return scaled(base, 0.75 + (r * 0.5)); // ±25 %
}

void ReconnectPolicy::waitAtLeast(Duration duration)
{
    m_notBefore = std::max(m_notBefore, m_clock.now() + duration);
}

void ReconnectPolicy::pruneAttempts() const
{
    const auto now = m_clock.now();
    while (!m_attempts.empty() && now - m_attempts.front() >= kAttemptWindow) {
        m_attempts.pop_front();
    }
}

Decision ReconnectPolicy::mayLogin() const
{
    Decision decision;
    if (m_needsUser) {
        decision.refusal = Refusal::LoginNeedsUser;
        decision.explanation = m_needsUserReason;
        return decision;
    }
    const auto now = m_clock.now();
    if (now < m_notBefore) {
        decision.refusal = Refusal::LoginBackoff;
        decision.retryAfter = m_notBefore - now;
        decision.explanation = u"Waiting %1 before reconnecting."_s.arg(describe(decision.retryAfter));
        return decision;
    }
    pruneAttempts();
    if (std::cmp_greater_equal(m_attempts.size(), limits::kLoginAttemptsPerHourCeiling)) {
        decision.refusal = Refusal::LoginBackoff;
        decision.retryAfter = m_attempts.front() + kAttemptWindow - now;
        decision.explanation = u"%1 logins in the last hour; waiting %2."_s.arg(limits::kLoginAttemptsPerHourCeiling)
                                   .arg(describe(decision.retryAfter));
        return decision;
    }
    decision.explanation = u"Login allowed."_s;
    return decision; // the Governor attaches the permit
}

void ReconnectPolicy::attemptStarted()
{
    pruneAttempts();
    m_attempts.push_back(m_clock.now());
}

void ReconnectPolicy::handle(Event event)
{
    const auto now = m_clock.now();
    switch (event) {
    case Event::LoginSucceeded:
        m_loggedInSince = now;
        return;
    case Event::ConnectionLost: {
        const bool stable = m_loggedInSince && now - *m_loggedInSince >= kStableSession;
        m_loggedInSince.reset();
        if (stable || m_delay == Duration::zero()) {
            const double r = std::clamp(m_random(), 0.0, 1.0);
            m_delay = limits::kReconnectFirstMin + scaled(limits::kReconnectFirstMax - limits::kReconnectFirstMin, r);
        } else {
            m_delay = std::min<Duration>(jittered(m_delay * 2), limits::kReconnectCap);
        }
        waitAtLeast(m_delay);
        return;
    }
    case Event::LoginTimeout:
        m_loggedInSince.reset();
        m_delay = std::clamp<Duration>(m_delay * 2, limits::kAfterLoginTimeoutWait, limits::kAfterLoginTimeoutCap);
        waitAtLeast(m_delay);
        return;
    case Event::InstantEof:
        m_loggedInSince.reset();
        waitAtLeast(kInstantEofWait + kBanJitterMin + scaled(kBanJitterSpan, std::clamp(m_random(), 0.0, 1.0)));
        return;
    case Event::BanNotice:
        m_loggedInSince.reset();
        waitAtLeast(limits::kAfterBanWait + kBanJitterMin + scaled(kBanJitterSpan, std::clamp(m_random(), 0.0, 1.0)));
        m_needsUser = true;
        m_needsUserReason = u"The server banned this account for 30 minutes. Selecta paused the network; reconnect "
                            u"when you are ready."_s;
        return;
    case Event::Relogged:
        m_loggedInSince.reset();
        m_needsUser = true;
        m_needsUserReason = u"This account logged in somewhere else. Selecta won't reconnect on its own."_s;
        return;
    case Event::LoginRejected:
        m_loggedInSince.reset();
        m_needsUser = true;
        m_needsUserReason = u"The server refused the login. Check the user name and password, then connect again."_s;
        return;
    case Event::ServerFull:
        m_loggedInSince.reset();
        m_delay = std::max<Duration>(m_delay * 2, kServerFullWait);
        m_delay = std::min<Duration>(m_delay, limits::kAfterLoginTimeoutCap);
        waitAtLeast(m_delay);
        return;
    case Event::ProtocolViolation:
        // Retrying can't help, and a codec bug must never turn into hammering a server.
        m_loggedInSince.reset();
        m_needsUser = true;
        m_needsUserReason = u"The server sent something Selecta couldn't read, so it disconnected. Connect again "
                            u"when you are ready."_s;
        return;
    case Event::UserRequested:
        // The user can lift the "needs user" block, but not a ban's waiting time or the hourly cap.
        m_needsUser = false;
        m_needsUserReason.clear();
        return;
    }
}

} // namespace selecta::governor
