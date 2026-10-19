// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SourceListView.h"

namespace selecta::app {
namespace {

bool isSelectable(const QModelIndex &index)
{
    return index.isValid() && index.flags().testFlag(Qt::ItemIsSelectable);
}

} // namespace

SourceListView::SourceListView(QWidget *parent)
    : QListView(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setUniformItemSizes(false);
    setMouseTracking(true);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setBackgroundRole(QPalette::Window);
    viewport()->setBackgroundRole(QPalette::Window);
}

QModelIndex SourceListView::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers)
{
    const QModelIndex current = currentIndex();
    QAbstractItemModel *itemModel = model();
    if (!itemModel)
        return {};
    const int rows = itemModel->rowCount(rootIndex());
    if (rows == 0)
        return {};

    int step = 0;
    int start = current.isValid() ? current.row() : -1;
    switch (cursorAction) {
    case MoveUp:
    case MovePrevious:
    case MoveLeft:
        step = -1;
        break;
    case MoveDown:
    case MoveNext:
    case MoveRight:
        step = 1;
        break;
    case MoveHome:
        step = 1;
        start = -1;
        break;
    case MoveEnd:
        step = -1;
        start = rows;
        break;
    case MovePageUp:
    case MovePageDown: {
        // Let QListView page, then settle on the nearest selectable row in the same direction.
        const QModelIndex paged = QListView::moveCursor(cursorAction, modifiers);
        if (isSelectable(paged))
            return paged;
        step = cursorAction == MovePageUp ? -1 : 1;
        start = paged.isValid() ? paged.row() : start;
        break;
    }
    }

    for (int row = start + step; row >= 0 && row < rows; row += step) {
        const QModelIndex candidate = itemModel->index(row, 0, rootIndex());
        if (isSelectable(candidate))
            return candidate;
    }
    return isSelectable(current) ? current : QModelIndex();
}

} // namespace selecta::app
