// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/PeerStats.h"

#include <QAbstractTableModel>
#include <QList>

namespace selecta::app {

// The peers Selecta took files from (PRD FAIR-1): today (what the daily limits count), this session, all time, the
// last download, what waits in their queue, and today against the daily limit, with a meter. Main thread only.
class PeersModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        PeerColumn,
        TodayColumn,
        SessionColumn,
        AllTimeColumn,
        LastColumn,
        StatusColumn,
        LimitColumn,
        ColumnCount
    };

    explicit PeersModel(QObject *parent = nullptr);

    // The peers now, as of `nowMs` (Unix time, ms; for "12 min ago").
    void reset(const QList<core::PeerStats> &peers, qint64 nowMs);
    core::PeerStats peerAt(int row) const { return m_peers.value(row); }
    // "2 queued · 1 arriving", "Holding off for 23 h", "Daily limit reached".
    static QString statusText(const core::PeerStats &peer);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    QVariant display(const core::PeerStats &peer, int column) const;

    QList<core::PeerStats> m_peers;
    qint64 m_nowMs = 0;
};

} // namespace selecta::app
