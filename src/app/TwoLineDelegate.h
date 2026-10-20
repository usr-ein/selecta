// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStyledItemDelegate>

namespace selecta::app {

// Paints a cell as two lines: the display text, and under it a smaller secondary line from `subtitleRole` in the
// placeholder colour (a track and its matched release, for instance). Elides both to the cell width.
class TwoLineDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit TwoLineDelegate(int subtitleRole, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    int m_subtitleRole;
};

} // namespace selecta::app
