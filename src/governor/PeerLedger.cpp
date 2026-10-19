// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/PeerLedger.h"

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::governor {
namespace {

constexpr auto kBannedBackoff = std::chrono::days(30);
constexpr auto kNotSharedBackoff = 24h;
constexpr auto kNotSharedWindow = 24h;
constexpr auto kShutdownBackoff = 15min;

// Peers reply with exact strings (protocol note §6.5); anything mentioning a ban counts as one.
bool isBan(const QString &reason)
{
    return reason.compare(u"Banned"_s, Qt::CaseInsensitive) == 0 || reason.contains(u"banned"_s, Qt::CaseInsensitive);
}

} // namespace

PeerLedger::PeerLedger(const base::Clock &clock, std::function<QDate()> today)
    : m_clock(clock)
    , m_today(std::move(today))
{}

PeerLedger::Peer &PeerLedger::peer(const QString &name)
{
    return m_peers[name];
}

const PeerLedger::Peer *PeerLedger::find(const QString &name) const
{
    const auto found = m_peers.constFind(name);
    return found == m_peers.cend() ? nullptr : &found.value();
}

void PeerLedger::pruneDays(Peer &entry) const
{
    const QDate oldest = m_today().addDays(-kDaysKept);
    entry.days.removeIf([&](const auto &item) { return item.key() < oldest; });
}

PeerLedger::DayTotals PeerLedger::recordCompleted(const QString &name, qint64 bytes)
{
    Peer &entry = peer(name);
    const QDate today = m_today();
    PeerDay &day = entry.days[today];
    day.files += 1;
    day.bytes += std::max<qint64>(0, bytes);
    const DayTotals result{.day = today, .totals = day};
    pruneDays(entry);
    return result;
}

void PeerLedger::restoreDay(const QString &name, QDate day, PeerDay totals)
{
    if (!day.isValid() || day < m_today().addDays(-kDaysKept)) {
        return;
    }
    Peer &entry = peer(name);
    PeerDay &stored = entry.days[day];
    stored.files = std::max(stored.files, totals.files);
    stored.bytes = std::max(stored.bytes, totals.bytes);
}

PeerDay PeerLedger::today(const QString &name) const
{
    const Peer *entry = find(name);
    return entry ? entry->days.value(m_today()) : PeerDay{};
}

int PeerLedger::filesLastSevenDays(const QString &name) const
{
    const Peer *entry = find(name);
    if (!entry) {
        return 0;
    }
    const QDate today = m_today();
    int files = 0;
    for (qint64 back = 0; back < 7; ++back) {
        files += entry->days.value(today.addDays(-back)).files;
    }
    return files;
}

void PeerLedger::queued(const QString &name)
{
    peer(name).queued += 1;
    m_queuedTotal += 1;
}

void PeerLedger::started(const QString &name)
{
    Peer &entry = peer(name);
    if (entry.queued > 0) {
        entry.queued -= 1;
        m_queuedTotal -= 1;
    }
    entry.active += 1;
    m_activeTotal += 1;
}

void PeerLedger::finished(const QString &name)
{
    Peer &entry = peer(name);
    if (entry.active > 0) {
        entry.active -= 1;
        m_activeTotal -= 1;
    }
}

void PeerLedger::withdrawn(const QString &name)
{
    Peer &entry = peer(name);
    if (entry.queued > 0) {
        entry.queued -= 1;
        m_queuedTotal -= 1;
    }
}

int PeerLedger::queuedAt(const QString &name) const
{
    const Peer *entry = find(name);
    return entry ? entry->queued : 0;
}

int PeerLedger::activeAt(const QString &name) const
{
    const Peer *entry = find(name);
    return entry ? entry->active : 0;
}

std::optional<base::Clock::Duration> PeerLedger::denied(const QString &name, const QString &reason)
{
    const auto now = m_clock.now();
    Peer &entry = peer(name);
    std::optional<base::Clock::Duration> backoff;
    const QString text = reason.trimmed();
    if (isBan(text)) {
        backoff = kBannedBackoff;
    } else if (text == u"File not shared."_s) {
        // Once can be an encoding mismatch (the caller retries in Latin-1); twice within a day is a pattern.
        if (entry.lastNotShared && now - *entry.lastNotShared < kNotSharedWindow) {
            backoff = kNotSharedBackoff;
        }
        entry.lastNotShared = now;
    } else if (text == u"Pending shutdown."_s) {
        backoff = kShutdownBackoff;
    }
    if (backoff) {
        const auto until = now + *backoff;
        entry.backoffUntil = entry.backoffUntil ? std::max(*entry.backoffUntil, until) : until;
    }
    return backoff;
}

std::optional<base::Clock::Duration> PeerLedger::backoffRemaining(const QString &name) const
{
    const Peer *entry = find(name);
    if (!entry || !entry->backoffUntil) {
        return std::nullopt;
    }
    const auto now = m_clock.now();
    if (now >= *entry->backoffUntil) {
        return std::nullopt;
    }
    return *entry->backoffUntil - now;
}

void PeerLedger::restoreBackoff(const QString &name, base::Clock::Duration remaining)
{
    if (remaining <= base::Clock::Duration::zero()) {
        return;
    }
    peer(name).backoffUntil = m_clock.now() + remaining;
}

} // namespace selecta::governor
