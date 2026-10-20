// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/PeersModel.h"

#include "app/Format.h"
#include "app/MeterDelegate.h"
#include "governor/Limits.h"

#include <QGuiApplication>
#include <QPalette>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

namespace limits = governor::limits;

constexpr qint64 kHourMs = qint64{3600} * 1000;

// 0 within the limits, 1 past the warning, 2 at the stop.
int level(const core::PeerStats &peer)
{
    if (peer.todayFiles >= limits::kPeerDailyStopFiles || peer.todayBytes >= limits::kPeerDailyStopBytes) {
        return 2;
    }
    if (peer.todayFiles >= limits::kPeerDailyWarnFiles || peer.todayBytes >= limits::kPeerDailyWarnBytes) {
        return 1;
    }
    return 0;
}

QString filesAndSize(int files, qint64 bytes)
{
    if (files == 0) {
        return u"0"_s;
    }
    return bytes > 0 ? u"%1 · %2"_s.arg(files).arg(format::size(bytes)) : QString::number(files);
}

} // namespace

PeersModel::PeersModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

void PeersModel::reset(const QList<core::PeerStats> &peers, qint64 nowMs)
{
    beginResetModel();
    m_peers = peers;
    m_nowMs = nowMs;
    endResetModel();
}

QString PeersModel::statusText(const core::PeerStats &peer)
{
    if (peer.backoffMs > 0) {
        const qint64 hours = (peer.backoffMs + kHourMs - 1) / kHourMs;
        return hours > 48 ? tr("Holding off for %1 days").arg((hours + 23) / 24)
                          : tr("Holding off for %1 h").arg(std::max<qint64>(hours, 1));
    }
    switch (level(peer)) {
    case 2:
        return tr("Daily limit reached");
    case 1:
        return tr("Near the daily limit");
    default:
        break;
    }
    QStringList parts;
    if (peer.active > 0) {
        parts.append(tr("%1 arriving").arg(peer.active));
    }
    if (peer.queued > 0) {
        parts.append(tr("%1 queued").arg(peer.queued));
    }
    return parts.join(u" · "_s);
}

int PeersModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_peers.size());
}

int PeersModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PeersModel::display(const core::PeerStats &peer, int column) const
{
    switch (column) {
    case PeerColumn:
        return peer.peer;
    case TodayColumn:
        return filesAndSize(peer.todayFiles, peer.todayBytes);
    case SessionColumn:
        return peer.sessionFiles;
    case AllTimeColumn:
        return filesAndSize(peer.allFiles, peer.allBytes);
    case LastColumn:
        return peer.lastMs > 0 ? format::ago(peer.lastMs, m_nowMs) : QString();
    case StatusColumn:
        return statusText(peer);
    default:
        return {};
    }
}

QVariant PeersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_peers.size()) {
        return {};
    }
    const core::PeerStats &peer = m_peers.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return display(peer, index.column());
    case MeterDelegate::FractionRole:
        return index.column() == LimitColumn
                   ? QVariant(static_cast<double>(peer.todayFiles) / limits::kPeerDailyStopFiles)
                   : QVariant();
    case MeterDelegate::LevelRole:
        return level(peer);
    case Qt::ToolTipRole:
        if (index.column() == LimitColumn || index.column() == TodayColumn) {
            return tr("%1 files from %2 today, previews included. Selecta warns at %3 files (or %4) a day from one "
                      "peer and stops at %5 (or %6).")
                .arg(peer.todayFiles)
                .arg(peer.peer)
                .arg(limits::kPeerDailyWarnFiles)
                .arg(format::size(limits::kPeerDailyWarnBytes))
                .arg(limits::kPeerDailyStopFiles)
                .arg(format::size(limits::kPeerDailyStopBytes));
        }
        return {};
    case Qt::TextAlignmentRole:
        return index.column() == SessionColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case Qt::ForegroundRole:
        if (index.column() == StatusColumn && (peer.backoffMs > 0 || level(peer) > 0)) {
            return QColor(230, 140, 20);
        }
        if (index.column() == LastColumn) {
            return QGuiApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    default:
        return {};
    }
}

QVariant PeersModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case PeerColumn:
        return tr("Peer");
    case TodayColumn:
        return tr("Today");
    case SessionColumn:
        return tr("Session");
    case AllTimeColumn:
        return tr("All time");
    case LastColumn:
        return tr("Last");
    case StatusColumn:
        return tr("Now");
    case LimitColumn:
        return tr("Today vs limit");
    default:
        return {};
    }
}

} // namespace selecta::app
