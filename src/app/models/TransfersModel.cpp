// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/TransfersModel.h"

#include "app/Format.h"
#include "app/MeterDelegate.h"
#include "core/SearchResults.h"

#include <QGuiApplication>
#include <QPalette>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using State = core::Download::State;

constexpr quintptr kTopLevel = 0; // internalId of a group row; a file row has its group's index + 1
constexpr double kSmoothing = 0.25;

bool isWaiting(State state)
{
    return state == State::Waiting || state == State::Queued;
}

} // namespace

TransfersModel::TransfersModel(QObject *parent)
    : QAbstractItemModel(parent)
{}

void TransfersModel::setGrouping(Grouping grouping)
{
    if (grouping == m_grouping) {
        return;
    }
    beginResetModel();
    m_grouping = grouping;
    rebuild();
    endResetModel();
}

void TransfersModel::reset(const QList<core::Download> &downloads,
                           const QHash<qint64, std::pair<qint64, qint64>> &progress)
{
    beginResetModel();
    m_rows.clear();
    m_dirty.clear();
    for (const core::Download &download : downloads) {
        Row row;
        row.download = download;
        const auto [done, total] = progress.value(download.id, {download.received, download.size});
        row.done = done;
        row.total = total;
        m_rows.insert(download.id, row);
    }
    rebuild();
    endResetModel();
}

QString TransfersModel::groupKey(const core::Download &download) const
{
    if (m_grouping == Grouping::Peer) {
        return download.peer;
    }
    return download.peer + u'\x1f' + core::remoteFolderOf(core::decodeRemotePath(download.remotePath));
}

void TransfersModel::rebuild()
{
    m_groups.clear();
    QList<qint64> ids = m_rows.keys();
    std::ranges::sort(ids);
    for (const qint64 id : std::as_const(ids)) {
        place(id, false);
    }
    if (!m_uploads.isEmpty()) {
        m_groups.append(Group{.key = {}, .title = tr("Uploads"), .peer = {}, .members = {}, .uploads = true});
    }
}

void TransfersModel::place(qint64 id, bool announce)
{
    const core::Download &download = m_rows.value(id).download;
    const QString key = groupKey(download);
    const bool uploadsLast = !m_groups.isEmpty() && m_groups.constLast().uploads;
    const auto found =
        std::ranges::find_if(m_groups, [&key](const Group &group) { return !group.uploads && group.key == key; });
    if (found == m_groups.end()) {
        Group group;
        group.key = key;
        group.peer = download.peer;
        if (m_grouping == Grouping::Peer) {
            group.title = download.peer;
        } else {
            const QString folder = core::remoteFolderOf(core::decodeRemotePath(download.remotePath));
            group.title = folder.isEmpty() ? tr("Loose files") : core::remoteNameOf(folder);
        }
        group.members.append(id);
        const auto at = static_cast<int>(uploadsLast ? m_groups.size() - 1 : m_groups.size());
        if (announce) {
            beginInsertRows({}, at, at);
        }
        m_groups.insert(at, group);
        if (announce) {
            endInsertRows();
        }
        return;
    }
    const auto groupRow = static_cast<int>(std::distance(m_groups.begin(), found));
    const auto at = static_cast<int>(found->members.size()); // ids only grow: a new download goes last
    if (announce) {
        beginInsertRows(index(groupRow, 0), at, at);
    }
    found->members.append(id);
    if (announce) {
        endInsertRows();
    }
}

int TransfersModel::groupOf(qint64 id) const
{
    for (qsizetype i = 0; i < m_groups.size(); ++i) {
        if (m_groups.at(i).members.contains(id)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TransfersModel::update(const core::Download &download)
{
    const auto found = m_rows.find(download.id);
    if (found == m_rows.end()) {
        Row row;
        row.download = download;
        row.done = download.received;
        row.total = download.size;
        m_rows.insert(download.id, row);
        place(download.id, true);
        return;
    }
    found->download = download;
    if (download.state != State::Transferring) {
        found->done = download.received;
        found->total = download.size;
        found->speed = 0.0;
        found->sampleBytes = -1;
    }
    const int group = groupOf(download.id);
    if (group < 0) {
        return;
    }
    const auto row = static_cast<int>(m_groups.at(group).members.indexOf(download.id));
    const QModelIndex parent = index(group, 0);
    Q_EMIT dataChanged(index(row, 0, parent), index(row, ColumnCount - 1, parent));
    Q_EMIT dataChanged(index(group, 0), index(group, ColumnCount - 1));
}

void TransfersModel::progress(qint64 id, qint64 bytesDone, qint64 bytesTotal, qint64 nowMs)
{
    const auto found = m_rows.find(id);
    if (found == m_rows.end()) {
        return;
    }
    Row &row = *found;
    if (row.sampleBytes >= 0 && nowMs > row.sampleMs && bytesDone >= row.sampleBytes) {
        const double instant =
            static_cast<double>(bytesDone - row.sampleBytes) * 1000.0 / static_cast<double>(nowMs - row.sampleMs);
        row.speed = row.speed <= 0.0 ? instant : ((1.0 - kSmoothing) * row.speed) + (kSmoothing * instant);
    }
    row.sampleBytes = bytesDone;
    row.sampleMs = nowMs;
    row.done = bytesDone;
    if (bytesTotal > 0) {
        row.total = bytesTotal;
    }
    m_dirty.insert(id);
}

void TransfersModel::setUploads(const QList<core::UploadEntry> &uploads)
{
    // Few and short-lived: shown again whole.
    beginResetModel();
    m_uploads = uploads;
    if (!m_groups.isEmpty() && m_groups.constLast().uploads) {
        m_groups.removeLast();
    }
    if (!m_uploads.isEmpty()) {
        m_groups.append(Group{.key = {}, .title = tr("Uploads"), .peer = {}, .members = {}, .uploads = true});
    }
    endResetModel();
}

void TransfersModel::flush()
{
    QSet<int> groups;
    for (const qint64 id : std::as_const(m_dirty)) {
        const int group = groupOf(id);
        if (group < 0) {
            continue;
        }
        const auto row = static_cast<int>(m_groups.at(group).members.indexOf(id));
        const QModelIndex parent = index(group, 0);
        Q_EMIT dataChanged(index(row, StatusColumn, parent), index(row, ProgressColumn, parent));
        groups.insert(group);
    }
    for (const int group : std::as_const(groups)) {
        Q_EMIT dataChanged(index(group, StatusColumn), index(group, ProgressColumn));
    }
    m_dirty.clear();
}

TransfersModel::Counts TransfersModel::counts() const
{
    Counts counts;
    for (const Row &row : m_rows) {
        if (row.download.state == State::Transferring) {
            ++counts.downloading;
        } else if (isWaiting(row.download.state)) {
            ++counts.waiting;
        }
    }
    counts.uploading = static_cast<int>(
        std::ranges::count_if(m_uploads, [](const core::UploadEntry &upload) { return upload.active; }));
    return counts;
}

std::optional<core::Download> TransfersModel::downloadAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.internalId() == kTopLevel) {
        return std::nullopt;
    }
    const auto group = static_cast<qsizetype>(index.internalId() - 1);
    if (group >= m_groups.size() || m_groups.at(group).uploads || index.row() >= m_groups.at(group).members.size()) {
        return std::nullopt;
    }
    return m_rows.value(m_groups.at(group).members.at(index.row())).download;
}

QList<qint64> TransfersModel::downloadsAt(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return {};
    }
    if (index.internalId() == kTopLevel) {
        return index.row() < m_groups.size() ? m_groups.at(index.row()).members : QList<qint64>();
    }
    const std::optional<core::Download> download = downloadAt(index);
    return download ? QList<qint64>{download->id} : QList<qint64>();
}

double TransfersModel::fraction(const Row &row) const
{
    if (row.download.state == State::Arrived || row.download.state == State::Converting) {
        return 1.0;
    }
    return row.total > 0 ? std::clamp(static_cast<double>(row.done) / static_cast<double>(row.total), 0.0, 1.0) : 0.0;
}

QString TransfersModel::statusText(qint64 id) const
{
    const auto found = m_rows.constFind(id);
    if (found == m_rows.cend()) {
        return {};
    }
    const Row &row = *found;
    const core::Download &download = row.download;
    switch (download.state) {
    case State::Waiting:
        return download.message.isEmpty() ? tr("Waiting") : download.message;
    case State::Queued:
        return download.message.isEmpty() ? tr("Queued at %1").arg(download.peer) : download.message;
    case State::Transferring: {
        QStringList parts{tr("%1 %").arg(static_cast<int>(fraction(row) * 100.0))};
        if (row.speed > 0.0) {
            parts.append(format::speed(static_cast<qint64>(row.speed)));
            const auto left = static_cast<qint64>(static_cast<double>(std::max<qint64>(0, row.total - row.done)) *
                                                  1000.0 / row.speed);
            parts.append(tr("%1 left").arg(format::length(std::max<qint64>(left, 1000))));
        }
        return parts.join(u" · "_s);
    }
    case State::Converting:
        return download.message.isEmpty() ? tr("Converting") : download.message;
    case State::Arrived:
        return tr("In your Inbox");
    case State::Failed:
        return download.message.isEmpty() ? tr("Failed") : tr("Failed: %1").arg(download.message);
    case State::Cancelled:
        return tr("Cancelled");
    }
    return {};
}

QModelIndex TransfersModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount) {
        return {};
    }
    if (!parent.isValid()) {
        return row < m_groups.size() ? createIndex(row, column, kTopLevel) : QModelIndex();
    }
    if (parent.internalId() != kTopLevel || parent.row() >= m_groups.size()) {
        return {};
    }
    const Group &group = m_groups.at(parent.row());
    const qsizetype count = group.uploads ? m_uploads.size() : group.members.size();
    return row < count ? createIndex(row, column, static_cast<quintptr>(parent.row()) + 1) : QModelIndex();
}

QModelIndex TransfersModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == kTopLevel) {
        return {};
    }
    return createIndex(static_cast<int>(child.internalId() - 1), 0, kTopLevel);
}

int TransfersModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return static_cast<int>(m_groups.size());
    }
    if (parent.internalId() != kTopLevel || parent.column() != 0 || parent.row() >= m_groups.size()) {
        return 0;
    }
    const Group &group = m_groups.at(parent.row());
    return static_cast<int>(group.uploads ? m_uploads.size() : group.members.size());
}

int TransfersModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    return ColumnCount;
}

QVariant TransfersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    QVariant value;
    if (index.internalId() == kTopLevel) {
        value = index.row() < m_groups.size() ? groupData(m_groups.at(index.row()), index.column(), role) : QVariant();
    } else if (const auto group = static_cast<qsizetype>(index.internalId() - 1); group >= m_groups.size()) {
        return {};
    } else if (m_groups.at(group).uploads) {
        value = index.row() < m_uploads.size() ? uploadData(m_uploads.at(index.row()), index.column(), role)
                                               : QVariant();
    } else {
        const QList<qint64> &members = m_groups.at(group).members;
        value = index.row() < members.size()
                    ? downloadData(m_rows.value(members.at(index.row())), index.column(), role)
                    : QVariant();
    }
    if (role == Qt::ToolTipRole && value.typeId() == QMetaType::QString) {
        return format::plainToolTip(value.toString()); // peers' names and paths, as they are
    }
    return value;
}

QVariant TransfersModel::downloadData(const Row &row, int column, int role) const
{
    const core::Download &download = row.download;
    if (role == DownloadIdRole) {
        return download.id;
    }
    if (role == IsGroupRole) {
        return false;
    }
    if (role == MeterDelegate::FractionRole && column == ProgressColumn) {
        return download.state == State::Failed || download.state == State::Cancelled ? -1.0 : fraction(row);
    }
    if (role == Qt::DisplayRole) {
        switch (column) {
        case ItemColumn:
            return core::remoteNameOf(core::decodeRemotePath(download.remotePath));
        case PeerColumn:
            return download.peer;
        case StatusColumn:
            return statusText(download.id);
        case ProgressColumn:
            return download.state == State::Failed || download.state == State::Cancelled
                       ? QVariant()
                       : QVariant(tr("%1 %").arg(static_cast<int>(fraction(row) * 100.0)));
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        return column == StatusColumn ? statusText(download.id) : QVariant(core::decodeRemotePath(download.remotePath));
    }
    if (role == Qt::ForegroundRole && column == StatusColumn && download.state != State::Failed) {
        return QGuiApplication::palette().brush(QPalette::PlaceholderText);
    }
    return {};
}

TransfersModel::GroupSummary TransfersModel::summarise(const Group &group) const
{
    GroupSummary summary;
    for (const qint64 id : group.members) {
        const auto found = m_rows.constFind(id);
        if (found == m_rows.cend()) {
            continue;
        }
        const Row &row = *found;
        const State state = row.download.state;
        summary.arrived += state == State::Arrived ? 1 : 0;
        summary.failed += state == State::Failed ? 1 : 0;
        summary.arriving += state == State::Transferring ? 1 : 0;
        if (state != State::Failed && state != State::Cancelled) {
            const qint64 size = std::max<qint64>(row.total, 0);
            summary.total += size;
            summary.done +=
                state == State::Arrived || state == State::Converting ? size : std::max<qint64>(row.done, 0);
        }
    }
    return summary;
}

QVariant TransfersModel::groupData(const Group &group, int column, int role) const
{
    if (role == DownloadIdRole) {
        return qint64{-1};
    }
    if (role == IsGroupRole) {
        return true;
    }
    if (group.uploads) {
        if (role == Qt::DisplayRole && column == ItemColumn) {
            return m_uploads.size() == 1 ? tr("Uploads · 1 file") : tr("Uploads · %1 files").arg(m_uploads.size());
        }
        return {};
    }
    const GroupSummary summary = summarise(group);
    const auto count = static_cast<int>(group.members.size());
    if (role == MeterDelegate::FractionRole && column == ProgressColumn) {
        return summary.total > 0 ? static_cast<double>(summary.done) / static_cast<double>(summary.total) : -1.0;
    }
    if (role == Qt::AccessibleTextRole && column == ProgressColumn && summary.total > 0) { // the meter, in words
        return tr("%1 %").arg(
            static_cast<int>(static_cast<double>(summary.done) / static_cast<double>(summary.total) * 100.0));
    }
    if (role == Qt::DisplayRole) {
        switch (column) {
        case ItemColumn:
            return count == 1 ? tr("%1 · 1 file").arg(group.title) : tr("%1 · %2 files").arg(group.title).arg(count);
        case PeerColumn:
            return m_grouping == Grouping::Folder ? group.peer : QString();
        case StatusColumn: {
            QStringList parts;
            parts.append(tr("%1 of %2 in your Inbox").arg(summary.arrived).arg(count));
            if (summary.arriving > 0) {
                parts.append(tr("%1 arriving").arg(summary.arriving));
            }
            if (summary.failed > 0) {
                parts.append(tr("%1 failed").arg(summary.failed));
            }
            return parts.join(u" · "_s);
        }
        default:
            return {};
        }
    }
    if (role == Qt::FontRole && column == ItemColumn) {
        QFont bold;
        bold.setBold(true);
        return bold;
    }
    if (role == Qt::ForegroundRole && column == StatusColumn) {
        return QGuiApplication::palette().brush(QPalette::PlaceholderText);
    }
    return {};
}

QVariant TransfersModel::uploadData(const core::UploadEntry &upload, int column, int role) const
{
    if (role == DownloadIdRole) {
        return qint64{-1};
    }
    if (role == IsGroupRole) {
        return false;
    }
    const double done = upload.size > 0 ? static_cast<double>(upload.sent) / static_cast<double>(upload.size) : 0.0;
    if (role == MeterDelegate::FractionRole && column == ProgressColumn) {
        return upload.active ? std::clamp(done, 0.0, 1.0) : -1.0;
    }
    if (role == Qt::AccessibleTextRole && column == ProgressColumn && upload.active) { // the meter, in words
        return tr("%1 %").arg(static_cast<int>(std::clamp(done, 0.0, 1.0) * 100.0));
    }
    if (role == Qt::DisplayRole) {
        switch (column) {
        case ItemColumn:
            return core::remoteNameOf(upload.path);
        case PeerColumn:
            return upload.user;
        case StatusColumn:
            return upload.active ? tr("Uploading · %1 of %2")
                                       .arg(format::size(static_cast<qint64>(upload.sent)),
                                            format::size(static_cast<qint64>(upload.size)))
                                 : tr("Queued here");
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        return upload.path;
    }
    return {};
}

QVariant TransfersModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case ItemColumn:
        return tr("Item");
    case PeerColumn:
        return tr("Peer");
    case StatusColumn:
        return tr("Status");
    case ProgressColumn:
        return tr("Progress");
    default:
        return {};
    }
}

Qt::ItemFlags TransfersModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

} // namespace selecta::app
