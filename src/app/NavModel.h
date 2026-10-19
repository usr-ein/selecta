// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QIcon>
#include <QList>
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
    };

    struct Item
    {
        QString id;    // stable identifier used by navigation, settings and the debug protocol
        QString title; // empty for a spacer header
        QIcon icon;
        bool isHeader = false;
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

private:
    static QList<Item> defaultItems();

    QList<Item> m_items;
    QHash<QString, int> m_counts;
};

} // namespace selecta::app
