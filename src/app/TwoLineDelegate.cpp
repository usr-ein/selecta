// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/TwoLineDelegate.h"

#include <QApplication>
#include <QPainter>

namespace selecta::app {
namespace {

constexpr int kPadding = 4;
constexpr qreal kSubtitleScale = 0.86;

QFont subtitleFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * kSubtitleScale);
    return font;
}

} // namespace

TwoLineDelegate::TwoLineDelegate(int subtitleRole, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_subtitleRole(subtitleRole)
{}

void TwoLineDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString title = opt.text;
    const QString subtitle = index.data(m_subtitleRole).toString();
    opt.text.clear();
    const QWidget *widget = opt.widget;
    const QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget); // background, selection, focus

    const QRect area = opt.rect.adjusted(kPadding, kPadding / 2, -kPadding, -kPadding / 2);
    const bool selected = (opt.state & QStyle::State_Selected) != 0;
    const QPalette::ColorGroup group = (opt.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
    const QColor titleColour = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
    QColor subtitleColour = selected ? titleColour : opt.palette.color(group, QPalette::PlaceholderText);
    if (selected) {
        subtitleColour.setAlphaF(0.8);
    }

    const QFontMetrics titleMetrics(opt.font);
    const QFont smaller = subtitleFont(opt.font);
    const QFontMetrics subtitleMetrics(smaller);
    const int total = titleMetrics.height() + (subtitle.isEmpty() ? 0 : subtitleMetrics.height());
    int top = area.top() + std::max(0, (area.height() - total) / 2);

    painter->save();
    painter->setFont(opt.font);
    painter->setPen(titleColour);
    painter->drawText(QRect(area.left(), top, area.width(), titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      titleMetrics.elidedText(title, Qt::ElideRight, area.width()));
    top += titleMetrics.height();
    if (!subtitle.isEmpty()) {
        painter->setFont(smaller);
        painter->setPen(subtitleColour);
        painter->drawText(QRect(area.left(), top, area.width(), subtitleMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          subtitleMetrics.elidedText(subtitle, Qt::ElideRight, area.width()));
    }
    painter->restore();
}

QSize TwoLineDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const QFontMetrics titleMetrics(option.font);
    const QFontMetrics subtitleMetrics(subtitleFont(option.font));
    size.setHeight(std::max(size.height(), titleMetrics.height() + subtitleMetrics.height() + (kPadding * 2)));
    return size;
}

} // namespace selecta::app
