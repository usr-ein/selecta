// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/NavModel.h"

#include "app/Icons.h"

#include <QLocale>

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
        return entry.title;
    case Qt::AccessibleTextRole:
        return accessibleText(entry);
    case Qt::AccessibleDescriptionRole:
        return entry.closable ? tr("Delete closes it.") : QString();
    case Qt::DecorationRole:
        return entry.icon;
    case IdRole:
        return entry.id;
    case IsHeaderRole:
        return entry.isHeader;
    case CountRole:
        return entry.isHeader ? 0 : m_counts.value(entry.id, 0);
    case ClosableRole:
        return entry.closable;
    case BusyRole:
        return m_busy.contains(entry.id);
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

QString NavModel::accessibleText(const Item &entry) const
{
    // What the delegate draws beside the title, in words (UI-5): "Messages, 3 unread", "neon rain, 214 files,
    // searching".
    QStringList parts{entry.title};
    const int count = entry.isHeader ? 0 : m_counts.value(entry.id, 0);
    const QString number = QLocale().toString(count);
    if (count > 0 && (entry.id == u"messages"_s || entry.id == u"rooms"_s)) {
        parts.append(tr("%1 unread").arg(number));
    } else if (count > 0 && entry.id == u"wishlist"_s) {
        parts.append(count == 1 ? tr("1 new file") : tr("%1 new files").arg(number));
    } else if (count > 0) {
        parts.append(count == 1 ? tr("1 file") : tr("%1 files").arg(number));
    }
    if (m_busy.contains(entry.id)) {
        parts.append(searchOfItem(entry.id) > 0 ? tr("searching") : tr("loading"));
    }
    return parts.join(u", "_s);
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
    Q_EMIT dataChanged(changed, changed, {CountRole, Qt::AccessibleTextRole});
}

QString NavModel::searchItemId(qint64 searchId)
{
    return u"search:%1"_s.arg(searchId);
}

qint64 NavModel::searchOfItem(const QString &id)
{
    if (!id.startsWith(u"search:"_s)) {
        return 0;
    }
    bool ok = false;
    const qint64 searchId = id.mid(7).toLongLong(&ok);
    return ok && searchId > 0 ? searchId : 0;
}

void NavModel::setSearch(qint64 searchId, const QString &title, int count, bool busy)
{
    setDiggingItem(searchItemId(searchId), title, icons::symbol(u"magnifyingglass"_s, u"edit-find"_s), count, busy);
}

void NavModel::removeSearch(qint64 searchId)
{
    removeItem(searchItemId(searchId));
}

void NavModel::setBrowse(const QString &peer, int count, bool busy)
{
    setDiggingItem(browseItemId(peer), peer, icons::symbol(u"folder"_s, u"folder"_s), count, busy);
}

void NavModel::removeBrowse(const QString &peer)
{
    removeItem(browseItemId(peer));
}

QString NavModel::browseItemId(const QString &peer)
{
    return u"browse:"_s + peer;
}

QString NavModel::browseOfItem(const QString &id)
{
    return id.startsWith(u"browse:"_s) ? id.mid(7) : QString();
}

void NavModel::setDiggingItem(const QString &id, const QString &title, const QIcon &icon, int count, bool busy)
{
    int row = rowOf(id);
    if (row < 0) {
        // Newest first, right under the "Digging" header.
        const int wishlist = rowOf(u"wishlist"_s);
        int at = wishlist >= 0 ? wishlist : static_cast<int>(m_items.size());
        while (at > 0 && !m_items.at(at - 1).isHeader && m_items.at(at - 1).closable) {
            --at;
        }
        beginInsertRows({}, at, at);
        m_items.insert(at, Item{.id = id, .title = title, .icon = icon, .isHeader = false, .closable = true});
        endInsertRows();
        row = at;
    }
    Item &entry = m_items[row];
    entry.title = title;
    if (count > 0) {
        m_counts.insert(id, count);
    } else {
        m_counts.remove(id);
    }
    if (busy) {
        m_busy.insert(id);
    } else {
        m_busy.remove(id);
    }
    const QModelIndex changed = index(row);
    Q_EMIT dataChanged(changed, changed);
}

void NavModel::removeItem(const QString &id)
{
    const int row = rowOf(id);
    if (row < 0) {
        return;
    }
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    m_counts.remove(id);
    m_busy.remove(id);
    endRemoveRows();
}

} // namespace selecta::app
