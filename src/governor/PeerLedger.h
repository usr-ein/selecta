// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"

#include <QDate>
#include <QHash>
#include <QString>

#include <functional>
#include <optional>

namespace selecta::governor {

// How much Selecta took from one peer on one calendar day (FAIR-2). Previews count too (PREV-7).
struct PeerDay
{
    int files = 0;
    qint64 bytes = 0;
};

// Per-peer bookkeeping for the fairness limits: completed downloads per day and over 7 days, files queued and active
// at each peer, and back-off after a peer's denial. Days are local calendar days from the injected `today` (so tests
// control midnight); back-off uses the monotonic clock. Not thread-safe; owned by the Governor.
class PeerLedger
{
public:
    PeerLedger(const base::Clock &clock, std::function<QDate()> today);

    // --- downloads completed (counted toward the daily and weekly caps) ---
    void recordCompleted(const QString &name, qint64 bytes);
    // Restores a day's totals saved by an earlier session (today's counters must survive a restart).
    void restoreDay(const QString &name, QDate day, PeerDay totals);
    PeerDay today(const QString &name) const;
    int filesLastSevenDays(const QString &name) const;

    // --- queue state at the peer ---
    void queued(const QString &name);    // QueueUpload sent
    void started(const QString &name);   // a queued file started transferring
    void finished(const QString &name);  // an active file ended (done, failed or cancelled)
    void withdrawn(const QString &name); // a queued file left the queue without starting
    int queuedAt(const QString &name) const;
    int activeAt(const QString &name) const;
    int queuedTotal() const { return m_queuedTotal; }
    int activeTotal() const { return m_activeTotal; }

    // --- denials ---
    // Records a peer's denial text and applies the back-off it calls for (etiquette note §9.1): "Banned" or any other
    // ban message 30 days; "File not shared." twice within a day 24 h; "Pending shutdown." 15 min. Returns the
    // back-off applied, if any.
    std::optional<base::Clock::Duration> denied(const QString &name, const QString &reason);
    // Remaining back-off for this peer, or nothing.
    std::optional<base::Clock::Duration> backoffRemaining(const QString &name) const;
    void restoreBackoff(const QString &name, base::Clock::Duration remaining);

private:
    struct Peer
    {
        QHash<QDate, PeerDay> days;
        int queued = 0;
        int active = 0;
        std::optional<base::Clock::TimePoint> backoffUntil;
        std::optional<base::Clock::TimePoint> lastNotShared;
    };
    Peer &peer(const QString &name);
    const Peer *find(const QString &name) const;
    void pruneDays(Peer &entry) const;

    const base::Clock &m_clock;
    std::function<QDate()> m_today;
    QHash<QString, Peer> m_peers;
    int m_queuedTotal = 0;
    int m_activeTotal = 0;
};

} // namespace selecta::governor
