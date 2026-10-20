// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PeerStats.h"

#include "governor/Governor.h"

#include <algorithm>
#include <chrono>

namespace selecta::core {

QList<PeerStats> peerStats(const QList<library::PeerTotals> &history, const QSet<QString> &current,
                           const governor::Governor &governor)
{
    const auto withCounters = [&governor](PeerStats stats) {
        const governor::PeerDay today = governor.peerToday(stats.peer);
        stats.todayFiles = today.files;
        stats.todayBytes = today.bytes;
        stats.queued = governor.peerQueued(stats.peer);
        stats.active = governor.peerActive(stats.peer);
        if (const auto backoff = governor.peerBackoff(stats.peer)) {
            stats.backoffMs = std::chrono::duration_cast<std::chrono::milliseconds>(*backoff).count();
        }
        return stats;
    };
    QList<PeerStats> stats;
    QSet<QString> listed;
    // Peers with a download under way first, by name: they're what the user watches.
    QStringList busy = current.values();
    std::ranges::sort(busy);
    for (const QString &peer : std::as_const(busy)) {
        if (!listed.contains(peer)) {
            PeerStats entry;
            entry.peer = peer;
            for (const library::PeerTotals &totals : history) {
                if (totals.peer == peer) {
                    entry.sessionFiles = totals.sessionFiles;
                    entry.allFiles = totals.files;
                    entry.allBytes = totals.bytes;
                    entry.lastMs = totals.lastMs;
                    break;
                }
            }
            stats.append(withCounters(entry));
            listed.insert(peer);
        }
    }
    for (const library::PeerTotals &totals : history) {
        if (listed.contains(totals.peer)) {
            continue;
        }
        PeerStats entry;
        entry.peer = totals.peer;
        entry.sessionFiles = totals.sessionFiles;
        entry.allFiles = totals.files;
        entry.allBytes = totals.bytes;
        entry.lastMs = totals.lastMs;
        stats.append(withCounters(entry));
        listed.insert(totals.peer);
    }
    return stats;
}

} // namespace selecta::core
