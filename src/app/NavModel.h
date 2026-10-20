// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QSet>
#include <QString>

namespace selecta::app {

// The sidebar's source list: section headers and navigable items, in the mock's order (PRD 8.2, D27).
class NavModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {                    // NOLINT(cppcoreguidelines-use-enum-class): Qt item roles are plain ints
        IdRole = Qt::UserRole + 1, // QString; empty for headers
        IsHeaderRole,              // bool
        CountRole,                 // int; 0 hides the count
        ClosableRole,              // bool: the item can be closed (a search)
        BusyRole,                  // bool: work in progress (a search collecting results)
    };

    struct Item
    {
        QString id;    // stable identifier used by navigation, settings and the debug protocol
        QString title; // empty for a spacer header
        QIcon icon;
        bool isHeader = false;
        bool closable = false;
    };

    explicit NavModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Row of the item with this id, or -1.
    int rowOf(const QString &id) const;
    QString idAt(int row) const;
    QString titleOf(const QString &id) const;
    bool isNavigable(int row) const;
    QStringList ids() const;

    // Sets the count shown next to an item (0 hides it). Unknown ids are ignored.
    void setCount(const QString &id, int count);

    // Searches typed by hand, under "Digging" before the Wishlist (PRD SRCH-1): added or updated, and removed.
    void setSearch(qint64 searchId, const QString &title, int count, bool busy);
    void removeSearch(qint64 searchId);
    static QString searchItemId(qint64 searchId);
    // The search behind an item id, or 0.
    static qint64 searchOfItem(const QString &id);
    // Peers' shares being browsed (PRD BRWS-1), with the searches: added or updated, and removed.
    void setBrowse(const QString &peer, int count, bool busy);
    void removeBrowse(const QString &peer);
    static QString browseItemId(const QString &peer);
    // The peer behind an item id, or an empty string.
    static QString browseOfItem(const QString &id);

private:
    static QList<Item> defaultItems();
    QString accessibleText(const Item &entry) const;
    // Adds or updates a closable item under "Digging", the newest first.
    void setDiggingItem(const QString &id, const QString &title, const QIcon &icon, int count, bool busy);
    void removeItem(const QString &id);

    QList<Item> m_items;
    QHash<QString, int> m_counts;
    QSet<QString> m_busy;
};

} // namespace selecta::app
