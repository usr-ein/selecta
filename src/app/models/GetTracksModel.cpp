// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/GetTracksModel.h"

#include "app/Format.h"

#include <QColor>
#include <QGuiApplication>
#include <QPalette>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using core::GetLine;
using ReleaseState = GetLine::ReleaseState;
using FindState = GetLine::FindState;
using DownloadState = GetLine::DownloadState;

bool darkPalette()
{
    return QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
}

// macOS system colours, readable on either appearance.
QColor colourFor(GetTracksModel::StatusKind kind)
{
    const bool dark = darkPalette();
    switch (kind) {
    case GetTracksModel::StatusKind::Good:
        return dark ? QColor(0x30, 0xD1, 0x58) : QColor(0x1E, 0x8E, 0x3E);
    case GetTracksModel::StatusKind::Check:
        return dark ? QColor(0xFF, 0x9F, 0x0A) : QColor(0xC9, 0x3C, 0x00);
    case GetTracksModel::StatusKind::Bad:
        return dark ? QColor(0xFF, 0x45, 0x3A) : QColor(0xD7, 0x00, 0x15);
    case GetTracksModel::StatusKind::Working:
        return dark ? QColor(0x0A, 0x84, 0xFF) : QColor(0x00, 0x63, 0xE1);
    case GetTracksModel::StatusKind::Waiting:
    case GetTracksModel::StatusKind::Have:
        break;
    }
    return QGuiApplication::palette().color(QPalette::PlaceholderText);
}

} // namespace

GetTracksModel::GetTracksModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

void GetTracksModel::reset(const QList<GetLine> &lines)
{
    beginResetModel();
    m_lines = lines;
    m_rows.clear();
    for (int row = 0; row < m_lines.size(); ++row) {
        m_rows.insert(m_lines.at(row).id, row);
    }
    endResetModel();
}

void GetTracksModel::update(const GetLine &line)
{
    const int row = rowOf(line.id);
    if (row < 0) {
        return;
    }
    m_lines[row] = line;
    Q_EMIT dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

const GetLine *GetTracksModel::lineAt(int row) const
{
    return row >= 0 && row < m_lines.size() ? &m_lines.at(row) : nullptr;
}

int GetTracksModel::rowOf(qint64 id) const
{
    return m_rows.value(id, -1);
}

GetTracksModel::Counts GetTracksModel::counts() const
{
    Counts counts;
    counts.total = static_cast<int>(m_lines.size());
    for (const GetLine &line : m_lines) {
        if (line.releaseState == ReleaseState::Pending) {
            ++(line.resolving ? counts.lookingUp : counts.waiting);
        }
        if (line.release && line.releaseState != ReleaseState::None) {
            ++counts.released;
        }
        switch (line.findState) {
        case FindState::Ready:
            ++counts.ready;
            break;
        case FindState::Check:
            ++counts.check;
            break;
        case FindState::NotFound:
            ++counts.notFound;
            break;
        case FindState::Owned:
        case FindState::InInbox:
            ++counts.have;
            break;
        case FindState::Idle:
        case FindState::Queued:
        case FindState::Searching:
            break;
        }
        if (line.ticked) {
            ++counts.ticked;
        }
    }
    return counts;
}

QString GetTracksModel::statusText(const GetLine &line)
{
    switch (line.downloadState) {
    case DownloadState::Queued:
        return tr("Queued at the peer");
    case DownloadState::Transferring:
        return tr("Downloading");
    case DownloadState::Converting:
        return tr("Converting");
    case DownloadState::Arrived:
        return tr("In your Inbox");
    case DownloadState::Failed:
        return tr("Download failed");
    case DownloadState::None:
        break;
    }
    switch (line.findState) {
    case FindState::Queued:
        return tr("Waiting to search");
    case FindState::Searching:
        return tr("Searching…");
    case FindState::Ready:
        return tr("Ready");
    case FindState::Check:
        return tr("Check");
    case FindState::NotFound:
        return tr("Not found");
    case FindState::Owned:
        return tr("You have it");
    case FindState::InInbox:
        return tr("In your Inbox");
    case FindState::Idle:
        break;
    }
    switch (line.releaseState) {
    case ReleaseState::Pending:
        return line.resolving ? tr("Looking up…") : tr("Waiting");
    case ReleaseState::Resolved:
        return tr("Release found");
    case ReleaseState::Ambiguous:
        return tr("Release unsure");
    case ReleaseState::None:
        return tr("No release");
    case ReleaseState::Failed:
        return tr("Lookup failed");
    }
    return {};
}

GetTracksModel::StatusKind GetTracksModel::statusKind(const GetLine &line)
{
    switch (line.downloadState) {
    case DownloadState::Queued:
    case DownloadState::Transferring:
    case DownloadState::Converting:
        return StatusKind::Working;
    case DownloadState::Arrived:
        return StatusKind::Good;
    case DownloadState::Failed:
        return StatusKind::Bad;
    case DownloadState::None:
        break;
    }
    switch (line.findState) {
    case FindState::Searching:
        return StatusKind::Working;
    case FindState::Ready:
        return StatusKind::Good;
    case FindState::Check:
        return StatusKind::Check;
    case FindState::NotFound:
        return StatusKind::Bad;
    case FindState::Owned:
    case FindState::InInbox:
        return StatusKind::Have;
    case FindState::Queued:
    case FindState::Idle:
        break;
    }
    switch (line.releaseState) {
    case ReleaseState::Pending:
        return line.resolving ? StatusKind::Working : StatusKind::Waiting;
    case ReleaseState::Ambiguous:
        return StatusKind::Check;
    case ReleaseState::Failed:
        return StatusKind::Bad;
    case ReleaseState::Resolved:
    case ReleaseState::None:
        break;
    }
    return StatusKind::Waiting;
}

QString GetTracksModel::subtitle(const GetLine &line)
{
    if (line.release && line.releaseState != ReleaseState::None) {
        return u"✓ "_s + format::release(*line.release);
    }
    switch (line.releaseState) {
    case ReleaseState::Pending:
        return line.resolving ? tr("Looking for the release…") : tr("Waiting for a release lookup (Spotify or Deezer)");
    case ReleaseState::None:
        return tr("No release found: Soulseek is searched by name, and tags come from the file");
    case ReleaseState::Failed:
        return line.message.isEmpty() ? tr("The lookup failed") : line.message;
    case ReleaseState::Resolved:
    case ReleaseState::Ambiguous:
        break;
    }
    return {};
}

int GetTracksModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_lines.size());
}

int GetTracksModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant GetTracksModel::data(const QModelIndex &index, int role) const
{
    const GetLine *line = index.isValid() ? lineAt(index.row()) : nullptr;
    if (!line) {
        return {};
    }
    if (role == LineIdRole) {
        return line->id;
    }
    if (role == StatusKindRole) {
        return static_cast<int>(statusKind(*line));
    }
    switch (index.column()) {
    case TickColumn:
        if (role == Qt::CheckStateRole && line->best) {
            return line->ticked ? Qt::Checked : Qt::Unchecked;
        }
        break;
    case TrackColumn:
        if (role == Qt::DisplayRole) {
            // The cleaned line: numbering, timestamps and labels stay in the tooltip's raw text.
            return line->artist.isEmpty() ? line->title : QString(line->artist + u" - "_s + line->title);
        }
        if (role == SubtitleRole) {
            return subtitle(*line);
        }
        if (role == Qt::ToolTipRole) {
            return QString(line->raw + u'\n' + subtitle(*line));
        }
        break;
    case StatusColumn:
        if (role == Qt::DisplayRole) {
            return statusText(*line);
        }
        if (role == Qt::ForegroundRole) {
            return colourFor(statusKind(*line));
        }
        if (role == Qt::ToolTipRole && !line->message.isEmpty()) {
            return line->message;
        }
        break;
    case FileColumn:
        if (!line->best) {
            break;
        }
        if (role == Qt::DisplayRole) {
            QString text = format::quality(*line->best);
            if (line->best->durationSec) {
                text += u" · "_s + format::length(qint64{*line->best->durationSec} * 1000);
            }
            return QString(text + u" · "_s + line->best->peer);
        }
        if (role == Qt::ToolTipRole) {
            return QString::fromUtf8(line->best->remotePath);
        }
        break;
    default:
        break;
    }
    return {};
}

bool GetTracksModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    const GetLine *line = index.isValid() ? lineAt(index.row()) : nullptr;
    if (!line || index.column() != TickColumn || role != Qt::CheckStateRole || !line->best) {
        return false;
    }
    Q_EMIT tickRequested(line->id, value.value<Qt::CheckState>() == Qt::Checked);
    return true;
}

Qt::ItemFlags GetTracksModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    const GetLine *line = index.isValid() ? lineAt(index.row()) : nullptr;
    if (line && index.column() == TickColumn && line->best) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}

QVariant GetTracksModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case TrackColumn:
        return tr("Track");
    case StatusColumn:
        return tr("Status");
    case FileColumn:
        return tr("Best file on Soulseek");
    default:
        return {};
    }
}

} // namespace selecta::app
