// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QPixmap>
#include <QStyledItemDelegate>

namespace selecta::app {

// Paints NavModel rows as a macOS source list: small section headers, items with a tinted symbol, a label and an
// optional count, and a rounded selection (accent when the list has focus, grey otherwise).
class NavDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit NavDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    QPixmap symbolPixmap(const QModelIndex &index, const QColor &color, qreal devicePixelRatio) const;

    mutable QHash<QString, QPixmap> m_pixmaps; // tinted symbols, keyed by id, colour and pixel ratio
};

} // namespace selecta::app
