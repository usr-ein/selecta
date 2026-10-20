// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStyledItemDelegate>

namespace selecta::app {

// A thin meter for a fraction (a transfer's progress, a peer's share of its daily limit): a rounded track with the
// filled part in the accent colour, or in orange or red past the warning levels the model gives. Cells without a
// fraction draw as text. Main thread.
class MeterDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    // double in [0, 1]; a cell without it (or with a negative value) is drawn as plain text.
    static constexpr int FractionRole = Qt::UserRole + 40;
    // int: 0 normal, 1 warning, 2 at the limit (the colour of the filled part).
    static constexpr int LevelRole = Qt::UserRole + 41;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace selecta::app
