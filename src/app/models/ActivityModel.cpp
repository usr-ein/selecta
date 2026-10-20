// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/ActivityModel.h"

#include <QColor>
#include <QDateTime>
#include <QGuiApplication>
#include <QLocale>
#include <QPalette>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

// Outcomes that mean "this went ahead"; everything else (refusals, failures, pauses) is worth a second look.
bool isFine(const QString &outcome)
{
    static const QStringList fine{u"allowed"_s, u"ok"_s, u"resumed"_s, u"connected"_s, u"cached"_s};
    return fine.contains(outcome);
}

QString formatTime(qint64 msSinceEpoch)
{
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(msSinceEpoch);
    const QLocale locale;
    if (when.date() == QDate::currentDate()) {
        return locale.toString(when.time(), u"HH:mm:ss"_s);
    }
    return locale.toString(when, QLocale::ShortFormat);
}

// macOS system orange, readable on both appearances.
QColor warningColour()
{
    const bool dark = QGuiApplication::palette().color(QPalette::Base).lightness() < 128;
    return dark ? QColor(0xFF, 0x9F, 0x0A) : QColor(0xC9, 0x3C, 0x00);
}

} // namespace

ActivityModel::ActivityModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

void ActivityModel::reset(const QList<core::ActivityEntry> &newestFirst)
{
    beginResetModel();
    m_entries = newestFirst.mid(0, kMaxRows);
    m_newestId = 0;
    for (const auto &entry : std::as_const(m_entries)) {
        m_newestId = std::max(m_newestId, entry.id);
    }
    endResetModel();
}

void ActivityModel::prepend(const core::ActivityEntry &entry)
{
    // Ids only grow, so anything at or below the newest id shown is already here (or was trimmed as too old).
    if (entry.id <= m_newestId) {
        return;
    }
    beginInsertRows({}, 0, 0);
    m_entries.prepend(entry);
    m_newestId = entry.id;
    endInsertRows();
    if (m_entries.size() > kMaxRows) {
        const int first = kMaxRows;
        const int last = static_cast<int>(m_entries.size()) - 1;
        beginRemoveRows({}, first, last);
        m_entries.resize(kMaxRows);
        endRemoveRows();
    }
}

core::ActivityEntry ActivityModel::entryAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row) : core::ActivityEntry{};
}

int ActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

int ActivityModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size()) {
        return {};
    }
    const core::ActivityEntry &entry = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Time:
            return formatTime(entry.atMs);
        case Operation:
            return humanise(entry.operation);
        case Subject:
            return entry.subject;
        case Outcome:
            return humanise(entry.outcome);
        default:
            return {};
        }
    case Qt::ToolTipRole:
        return entry.explanation.isEmpty() ? QVariant() : QVariant(entry.explanation);
    case Qt::ForegroundRole:
        if (index.column() == Outcome && isWarning(entry.outcome)) {
            return warningColour();
        }
        return {};
    case kEntryRole:
        return QVariant::fromValue(entry);
    default:
        return {};
    }
}

QVariant ActivityModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case Time:
        return tr("Time");
    case Operation:
        return tr("What");
    case Subject:
        return tr("About");
    case Outcome:
        return tr("Outcome");
    default:
        return {};
    }
}

QString ActivityModel::humanise(const QString &name)
{
    if (name.isEmpty()) {
        return name;
    }
    QString text = name;
    text.replace(u'-', u' ');
    text[0] = text[0].toUpper();
    return text;
}

bool ActivityModel::isWarning(const QString &outcome)
{
    return !outcome.isEmpty() && !isFine(outcome);
}

} // namespace selecta::app
