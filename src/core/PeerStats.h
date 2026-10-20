// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/HistoryStore.h"

#include <QList>
#include <QMetaType>
#include <QSet>
#include <QString>

namespace selecta::governor {
class Governor;
}

namespace selecta::core {

// What Selecta took from one peer (PRD FAIR-1), for the Peers view.
struct PeerStats
{
    QString peer;
    int todayFiles = 0; // the governor's count, previews included: what the daily limits apply to
    qint64 todayBytes = 0;
    int sessionFiles = 0; // downloads that arrived this session
    int allFiles = 0;     // downloads that arrived, all time
    qint64 allBytes = 0;
    qint64 lastMs = 0;    // the last download that arrived, Unix time in ms; 0: none yet
    int queued = 0;       // files waiting in its queue now
    int active = 0;       // files arriving now
    qint64 backoffMs = 0; // > 0: Selecta holds off this peer after a denial

    friend bool operator==(const PeerStats &, const PeerStats &) = default;
};

// Every peer in the history or with a download under way (`current`), the most recent first: the history's totals
// with the governor's counters for today, the queue and the back-off.
QList<PeerStats> peerStats(const QList<library::PeerTotals> &history, const QSet<QString> &current,
                           const governor::Governor &governor);

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::PeerStats)
