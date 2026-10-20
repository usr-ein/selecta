// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStyledItemDelegate>

namespace selecta::app {

// A tick box drawn by Selecta itself for checkable cells (Qt::CheckStateRole): an accent-coloured rounded square with
// a white tick when checked, an outline when not, nothing when the cell isn't checkable. The platform style draws
// item-view check boxes only in some window states, so a list of ticks could look empty. A click anywhere in the cell
// toggles it. Main thread.
class TickDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

protected:
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;
};

} // namespace selecta::app
