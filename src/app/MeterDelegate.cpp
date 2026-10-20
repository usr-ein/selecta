// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MeterDelegate.h"

#include <QApplication>
#include <QPainter>

#include <algorithm>

namespace selecta::app {
namespace {

constexpr qreal kHeight = 5.0;
constexpr int kMargin = 6;

} // namespace

void MeterDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QVariant fraction = index.data(FractionRole);
    if (!fraction.isValid() || fraction.toDouble() < 0.0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    // The row's background and selection, as any cell; then the meter on top.
    QStyleOptionViewItem background = option;
    initStyleOption(&background, index);
    background.text.clear();
    background.icon = {};
    const QWidget *widget = option.widget;
    const QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

    const QRectF track(option.rect.left() + kMargin, option.rect.center().y() - (kHeight / 2.0) + 0.5,
                       std::max(0, option.rect.width() - (2 * kMargin)), kHeight);
    QColor empty = option.palette.color(QPalette::PlaceholderText);
    empty.setAlphaF(0.25F);
    QColor filled = option.palette.color(QPalette::Accent);
    switch (index.data(LevelRole).toInt()) {
    case 1:
        filled = QColor(230, 140, 20);
        break;
    case 2:
        filled = QColor(215, 58, 50);
        break;
    default:
        break;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(empty);
    painter->drawRoundedRect(track, kHeight / 2.0, kHeight / 2.0);
    const qreal width = track.width() * std::clamp(fraction.toDouble(), 0.0, 1.0);
    if (width > 0.0) {
        painter->setBrush(filled);
        painter->drawRoundedRect(QRectF(track.left(), track.top(), std::max(width, kHeight), kHeight), kHeight / 2.0,
                                 kHeight / 2.0);
    }
    painter->restore();
}

} // namespace selecta::app
