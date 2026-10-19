// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/NavModel.h"

#include "app/Icons.h"

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

NavModel::Item header(const QString &title = {})
{
    return {.id = {}, .title = title, .icon = {}, .isHeader = true};
}

NavModel::Item item(const QString &id, const QString &title, const QString &sfSymbol, const QString &fallback)
{
    return {.id = id, .title = title, .icon = icons::symbol(sfSymbol, fallback), .isHeader = false};
}

} // namespace

NavModel::NavModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_items(defaultItems())
{}

QList<NavModel::Item> NavModel::defaultItems()
{
    return {
        item(u"get"_s, tr("Get tracks"), u"list.bullet.rectangle"_s, u"view-list-details"_s),
        item(u"inbox"_s, tr("Inbox"), u"tray.and.arrow.down"_s, u"mail-folder-inbox"_s),
        header(tr("Digging")),
        item(u"wishlist"_s, tr("Wishlist"), u"star"_s, u"starred"_s),
        header(tr("Library")),
        item(u"collection"_s, tr("Collection"), u"music.note.list"_s, u"folder-music"_s),
        item(u"history"_s, tr("History"), u"clock.arrow.circlepath"_s, u"document-open-recent"_s),
        header(tr("Network")),
        item(u"activity"_s, tr("Activity"), u"arrow.up.arrow.down"_s, u"network-transmit-receive"_s),
        item(u"shares"_s, tr("Shares"), u"dot.radiowaves.left.and.right"_s, u"folder-publicshare"_s),
        item(u"messages"_s, tr("Messages"), u"bubble.left.and.bubble.right"_s, u"mail-message"_s),
        item(u"rooms"_s, tr("Rooms"), u"person.3"_s, u"system-users"_s),
        header(),
        item(u"converter"_s, tr("Converter"), u"arrow.triangle.2.circlepath"_s, u"view-refresh"_s),
        item(u"settings"_s, tr("Settings"), u"slider.horizontal.3"_s, u"preferences-system"_s),
    };
}

int NavModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

QVariant NavModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const Item &entry = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
    case Qt::AccessibleTextRole:
        return entry.title;
    case Qt::DecorationRole:
        return entry.icon;
    case IdRole:
        return entry.id;
    case IsHeaderRole:
        return entry.isHeader;
    case CountRole:
        return entry.isHeader ? 0 : m_counts.value(entry.id, 0);
    default:
        return {};
    }
}

Qt::ItemFlags NavModel::flags(const QModelIndex &index) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return Qt::NoItemFlags;
    }
    return m_items.at(index.row()).isHeader ? Qt::NoItemFlags : (Qt::ItemIsEnabled | Qt::ItemIsSelectable);
}

int NavModel::rowOf(const QString &id) const
{
    if (id.isEmpty()) {
        return -1;
    }
    for (qsizetype row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == id) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString NavModel::idAt(int row) const
{
    return row >= 0 && row < m_items.size() ? m_items.at(row).id : QString();
}

QString NavModel::titleOf(const QString &id) const
{
    const int row = rowOf(id);
    return row < 0 ? QString() : m_items.at(row).title;
}

bool NavModel::isNavigable(int row) const
{
    return row >= 0 && row < m_items.size() && !m_items.at(row).isHeader;
}

QStringList NavModel::ids() const
{
    QStringList result;
    for (const Item &entry : m_items) {
        if (!entry.isHeader) {
            result.append(entry.id);
        }
    }
    return result;
}

void NavModel::setCount(const QString &id, int count)
{
    const int row = rowOf(id);
    if (row < 0) {
        return;
    }
    const int clamped = count > 0 ? count : 0;
    if (m_counts.value(id, 0) == clamped) {
        return;
    }
    if (clamped == 0) {
        m_counts.remove(id);
    } else {
        m_counts.insert(id, clamped);
    }
    const QModelIndex changed = index(row);
    Q_EMIT dataChanged(changed, changed, {CountRole});
}

} // namespace selecta::app
