// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/ConverterModel.h"

#include <QColor>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

using State = core::ConverterItem::State;

} // namespace

ConverterModel::ConverterModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

void ConverterModel::setItems(const QList<core::ConverterItem> &items)
{
    // Same files in the same order (the usual case: progress, a verdict): changed in place, so the selection stays.
    const bool sameRows =
        items.size() == m_items.size() &&
        std::ranges::equal(items, m_items,
                           [](const core::ConverterItem &a, const core::ConverterItem &b) { return a.id == b.id; });
    if (sameRows) {
        m_items = items;
        if (!m_items.isEmpty()) {
            Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(m_items.size()) - 1, ColumnCount - 1));
        }
        return;
    }
    beginResetModel();
    m_items = items;
    endResetModel();
}

std::optional<core::ConverterItem> ConverterModel::itemAt(int row) const
{
    if (row < 0 || row >= m_items.size()) {
        return std::nullopt;
    }
    return m_items.at(row);
}

int ConverterModel::rowOf(qint64 id) const
{
    for (qsizetype row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == id) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString ConverterModel::resultText(const core::ConverterItem &item)
{
    switch (item.state) {
    case State::Reading:
        return tr("Reading…");
    case State::Ready:
        if (item.refused) {
            return tr("Not offered: %1").arg(item.verdict);
        }
        return item.verdict;
    case State::Converting:
        return tr("Converting · %1 %").arg(static_cast<int>(item.progress * 100.0));
    case State::Done:
        return tr("Saved as %1").arg(QFileInfo(item.outputPath).fileName());
    case State::Failed:
        return tr("Failed: %1").arg(item.message);
    case State::Cancelled:
        return tr("Cancelled");
    }
    return {};
}

int ConverterModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

int ConverterModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConverterModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return {};
    }
    const core::ConverterItem &item = m_items.at(index.row());
    switch (role) {
    case ItemIdRole:
        return item.id;
    case Qt::DisplayRole:
        switch (index.column()) {
        case FileColumn:
            return QFileInfo(item.path).fileName();
        case QualityColumn:
            return item.quality;
        case ResultColumn:
            return resultText(item);
        default:
            return {};
        }
    case Qt::ToolTipRole:
        if (index.column() == FileColumn) {
            return item.path;
        }
        if (index.column() == ResultColumn) {
            return item.notices.isEmpty() ? resultText(item) : item.notices.join(u'\n');
        }
        return {};
    case Qt::CheckStateRole:
        if (index.column() == AnywayColumn && item.askFirst && item.state == State::Ready) {
            return item.confirmed ? Qt::Checked : Qt::Unchecked;
        }
        return {};
    case Qt::ForegroundRole:
        if (index.column() == ResultColumn && item.state == State::Ready && (item.askFirst || item.refused)) {
            return QColor(230, 140, 20);
        }
        if (index.column() == QualityColumn) {
            return QGuiApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    default:
        return {};
    }
}

bool ConverterModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.column() != AnywayColumn || role != Qt::CheckStateRole ||
        index.row() >= m_items.size() || !m_items.at(index.row()).askFirst) {
        return false;
    }
    const bool yes = value.value<Qt::CheckState>() == Qt::Checked;
    m_items[index.row()].confirmed = yes; // shown at once; the converter confirms it back
    Q_EMIT dataChanged(index, index);
    Q_EMIT confirmToggled(m_items.at(index.row()).id, yes);
    return true;
}

QVariant ConverterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case FileColumn:
        return tr("File");
    case QualityColumn:
        return tr("Quality");
    case ResultColumn:
        return tr("Result");
    case AnywayColumn:
        return tr("Convert anyway");
    default:
        return {};
    }
}

Qt::ItemFlags ConverterModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == AnywayColumn && index.row() < m_items.size() && m_items.at(index.row()).askFirst &&
        m_items.at(index.row()).state == State::Ready) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}

} // namespace selecta::app
