// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/TickDelegate.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace selecta::app {
namespace {

constexpr qreal kBox = 14.0;
constexpr qreal kRadius = 3.5;

bool checkable(const QModelIndex &index)
{
    return index.isValid() && (index.flags() & Qt::ItemIsUserCheckable) && index.data(Qt::CheckStateRole).isValid();
}

} // namespace

void TickDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // The row's background and selection, as any cell; then the box on top.
    QStyleOptionViewItem background = option;
    initStyleOption(&background, index);
    background.features &= ~QStyleOptionViewItem::HasCheckIndicator;
    background.text.clear();
    background.icon = {};
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
    if (!checkable(index)) {
        return;
    }
    const bool checked = index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
    const QRectF box(option.rect.center().x() - (kBox / 2.0) + 0.5, option.rect.center().y() - (kBox / 2.0) + 0.5, kBox,
                     kBox);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QPalette &palette = option.palette;
    if (checked) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(palette.color(QPalette::Accent));
        painter->drawRoundedRect(box, kRadius, kRadius);
        QPainterPath tick;
        tick.moveTo(box.left() + (box.width() * 0.25), box.top() + (box.height() * 0.53));
        tick.lineTo(box.left() + (box.width() * 0.43), box.top() + (box.height() * 0.71));
        tick.lineTo(box.left() + (box.width() * 0.76), box.top() + (box.height() * 0.32));
        painter->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(tick);
    } else {
        QColor outline = palette.color(QPalette::PlaceholderText);
        outline.setAlphaF(0.8F);
        painter->setPen(QPen(outline, 1.0));
        painter->setBrush(palette.color(QPalette::Base));
        painter->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    }
    painter->restore();
}

QSize TickDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QSize base = QStyledItemDelegate::sizeHint(option, index);
    return {static_cast<int>(kBox) + 12, std::max(base.height(), static_cast<int>(kBox) + 6)};
}

bool TickDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    if (!checkable(index) || model == nullptr) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = dynamic_cast<const QMouseEvent *>(event);
        if (mouse == nullptr || mouse->button() != Qt::LeftButton ||
            !option.rect.contains(mouse->position().toPoint())) {
            return false;
        }
        const bool checked = index.data(Qt::CheckStateRole).value<Qt::CheckState>() == Qt::Checked;
        return model->setData(index, checked ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
    }
    // Swallow the press and double-click so they don't also change the selection's current cell oddly.
    return event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick;
}

} // namespace selecta::app
