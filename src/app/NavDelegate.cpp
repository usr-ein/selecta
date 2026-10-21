// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/NavDelegate.h"

#include "app/Appearance.h"
#include "app/Icons.h"
#include "app/NavModel.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kItemHeight = 28;
constexpr int kHeaderHeight = 28;
constexpr int kSpacerHeight = 10;
constexpr int kSideInset = 8;
constexpr int kIconSize = 16;
constexpr int kMaxCachedPixmaps = 256;

QFont headerFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() - 2.0);
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

NavDelegate::NavDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{}

QSize NavDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const bool isHeader = index.data(NavModel::IsHeaderRole).toBool();
    if (!isHeader) {
        return {option.rect.width(), kItemHeight};
    }
    return {option.rect.width(), index.data(Qt::DisplayRole).toString().isEmpty() ? kSpacerHeight : kHeaderHeight};
}

QPixmap NavDelegate::symbolPixmap(const QModelIndex &index, const QColor &color, qreal devicePixelRatio) const
{
    const QString key = index.data(NavModel::IdRole).toString() + u'|' + QString::number(color.rgba(), 16) + u'|' +
                        QString::number(devicePixelRatio);
    if (const auto found = m_pixmaps.constFind(key); found != m_pixmaps.cend()) {
        return found.value();
    }
    if (m_pixmaps.size() >= kMaxCachedPixmaps) {
        m_pixmaps.clear(); // appearance changes create new keys; keep the cache bounded
    }
    const QPixmap pixmap = icons::tinted(index.data(Qt::DecorationRole).value<QIcon>(), QSize(kIconSize, kIconSize),
                                         devicePixelRatio, color);
    m_pixmaps.insert(key, pixmap);
    return pixmap;
}

void NavDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!painter || !index.isValid()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QPalette &palette = option.palette;
    const QRect rect = option.rect;

    if (index.data(NavModel::IsHeaderRole).toBool()) {
        const QString title = index.data(Qt::DisplayRole).toString();
        if (!title.isEmpty()) {
            painter->setFont(headerFont(option.font));
            painter->setPen(palette.color(QPalette::PlaceholderText));
            const QRect textRect = rect.adjusted(kSideInset + 6, 0, -kSideInset, -4);
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignBottom | Qt::TextSingleLine, title);
        }
        painter->restore();
        return;
    }

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool focused =
        selected && option.state.testFlag(QStyle::State_Active) && option.widget && option.widget->hasFocus();
    QColor textColor = palette.color(QPalette::Text);
    QColor iconColor = symbolAccentColor(palette); // the system accent, lighter in dark mode
    if (selected) {
        const QColor fill = focused ? palette.color(QPalette::Active, QPalette::Highlight)
                                    : palette.color(QPalette::Inactive, QPalette::Highlight);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect.adjusted(kSideInset, 1, -kSideInset, -1)), 6.0, 6.0);
        painter->fillPath(path, fill);
        if (focused) {
            textColor = palette.color(QPalette::Active, QPalette::HighlightedText);
            iconColor = textColor;
        }
    } else if (option.state.testFlag(QStyle::State_MouseOver)) {
        QColor hover = palette.color(QPalette::Text);
        hover.setAlphaF(0.06F);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect.adjusted(kSideInset, 1, -kSideInset, -1)), 6.0, 6.0);
        painter->fillPath(path, hover);
    }

    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : qApp->devicePixelRatio();
    const QPixmap symbol = symbolPixmap(index, iconColor, dpr);
    const int iconX = rect.left() + kSideInset + 8;
    const int iconY = rect.top() + ((rect.height() - kIconSize) / 2);
    if (!symbol.isNull()) {
        painter->drawPixmap(QPoint(iconX, iconY), symbol);
    }

    int right = rect.right() - kSideInset - 8;
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    if (index.data(NavModel::ClosableRole).toBool() && (hovered || selected)) {
        // A close button instead of the count, as Safari's sidebar tabs do.
        const QRect close = closeRect(rect);
        QColor mark = textColor;
        mark.setAlphaF(0.7F);
        QPen pen(mark, 1.4);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        const QRectF cross = QRectF(close).adjusted(4.5, 4.5, -4.5, -4.5);
        painter->drawLine(cross.topLeft(), cross.bottomRight());
        painter->drawLine(cross.topRight(), cross.bottomLeft());
        right = close.left() - 6;
    } else if (index.data(NavModel::BusyRole).toBool()) {
        QColor busy = textColor;
        busy.setAlphaF(0.55F);
        painter->setPen(busy);
        const QString dots = u"…"_s;
        const int width = option.fontMetrics.horizontalAdvance(dots);
        painter->drawText(QRect(right - width, rect.top(), width, rect.height()), Qt::AlignRight | Qt::AlignVCenter,
                          dots);
        right -= width + 8;
    }
    const int count = index.data(NavModel::CountRole).toInt();
    if (count > 0 && !(index.data(NavModel::ClosableRole).toBool() && (hovered || selected))) {
        QFont countFont = option.font;
        countFont.setPointSizeF(option.font.pointSizeF() - 1.0);
        painter->setFont(countFont);
        const QString countText = QLocale().toString(count);
        const int countWidth = QFontMetrics(countFont).horizontalAdvance(countText);
        QColor countColor = textColor;
        countColor.setAlphaF(selected && focused ? 0.85F : 0.55F);
        painter->setPen(countColor);
        painter->drawText(QRect(right - countWidth, rect.top(), countWidth, rect.height()),
                          Qt::AlignRight | Qt::AlignVCenter, countText);
        right -= countWidth + 8;
    }

    painter->setFont(option.font);
    painter->setPen(textColor);
    const int textLeft = iconX + kIconSize + 8;
    const QRect textRect(textLeft, rect.top(), std::max(0, right - textLeft), rect.height());
    const QString label =
        option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, label);
    painter->restore();
}

QRect NavDelegate::closeRect(QRect itemRect)
{
    constexpr int kCloseSize = 16;
    return {itemRect.right() - kSideInset - 6 - kCloseSize, itemRect.top() + ((itemRect.height() - kCloseSize) / 2),
            kCloseSize, kCloseSize};
}

bool NavDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                              const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease && index.data(NavModel::ClosableRole).toBool()) {
        const auto *mouse = dynamic_cast<const QMouseEvent *>(event);
        if (mouse && mouse->button() == Qt::LeftButton &&
            closeRect(option.rect).contains(mouse->position().toPoint())) {
            Q_EMIT closeRequested(index.data(NavModel::IdRole).toString());
            return true;
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace selecta::app
