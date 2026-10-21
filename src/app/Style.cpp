// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Style.h"

#include "app/SourceListView.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QPainter>
#include <QSplitter>
#include <QStyleOptionHeader>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <array>

namespace selecta::app::style {
namespace {

// Continuous corners as iOS 7 and macOS draw them (the curve PaintCode measured): along each side the corner starts
// 1.528 radii from the vertex, and three cubics ease from the side into the arc and out again. Coefficients in radii.
constexpr qreal kReach = 1.52866483; // where the curve leaves the straight side
constexpr qreal kC1 = 1.08849323;
constexpr qreal kC2 = 0.86840689;
constexpr qreal kC3 = 0.66993427;
constexpr qreal kC4 = 0.06549600;
constexpr qreal kC5 = 0.36493148;
constexpr qreal kC6 = 0.17093969;

// One corner at `vertex`: the side arriving runs along `in`, the side leaving along `out` (unit vectors). The path is
// at the arriving side's start of the curve.
void addCorner(QPainterPath &path, QPointF vertex, QPointF in, QPointF out, qreal radius)
{
    const auto at = [&](qreal back, qreal forward) { return vertex - (in * back * radius) + (out * forward * radius); };
    path.cubicTo(at(kC1, 0), at(kC2, 0), at(kC3, kC4));
    path.cubicTo(at(kC5, kC6), at(kC6, kC5), at(kC4, kC3));
    path.cubicTo(at(0, kC2), at(0, kC1), at(0, kReach));
}

const char *const kPlatformRows = "selectaPlatformRows"; // set by SelectaStyle::keepPlatformRows()
const char *const kHiddenHandles = "selectaHiddenHandles"; // set by SelectaStyle::hideHandles()

} // namespace

QPainterPath squircle(const QRectF &rect, qreal radius)
{
    return squircle(rect, radius, Corners::All);
}

QPainterPath squircle(const QRectF &rect, qreal radius, Corners corners)
{
    QPainterPath path;
    if (rect.isEmpty()) {
        return path;
    }
    const qreal shorter = std::min(rect.width(), rect.height());
    if (radius <= 0.0 || corners == Corners::None) {
        path.addRect(rect);
        return path;
    }
    if (corners == Corners::All && radius >= shorter / 2.0) {
        path.addRoundedRect(rect, shorter / 2.0, shorter / 2.0); // a capsule: round ends
        return path;
    }
    // The curve needs kReach radii of each side; on a small shape the radius gives way rather than the curve.
    const qreal r = std::min(radius, shorter / (2.0 * kReach));
    const bool left = corners == Corners::All || corners == Corners::Left;
    const bool right = corners == Corners::All || corners == Corners::Right;
    const QPointF topLeft = rect.topLeft();
    const QPointF topRight = rect.topRight();
    const QPointF bottomRight = rect.bottomRight();
    const QPointF bottomLeft = rect.bottomLeft();
    const QPointF east(1, 0);
    const QPointF south(0, 1);
    const QPointF west(-1, 0);
    const QPointF north(0, -1);

    path.moveTo(left ? topLeft + (east * kReach * r) : topLeft);
    if (right) {
        path.lineTo(topRight - (east * kReach * r));
        addCorner(path, topRight, east, south, r);
        path.lineTo(bottomRight - (south * kReach * r));
        addCorner(path, bottomRight, south, west, r);
    } else {
        path.lineTo(topRight);
        path.lineTo(bottomRight);
    }
    if (left) {
        path.lineTo(bottomLeft - (west * kReach * r));
        addCorner(path, bottomLeft, west, north, r);
        path.lineTo(topLeft - (north * kReach * r));
        addCorner(path, topLeft, north, east, r);
    } else {
        path.lineTo(bottomLeft);
        path.lineTo(topLeft);
    }
    path.closeSubpath();
    return path;
}

namespace {

bool startsRow(int position)
{
    return position == QStyleOptionViewItem::Beginning || position == QStyleOptionViewItem::OnlyOne ||
           position == QStyleOptionViewItem::Invalid;
}

bool endsRow(int position)
{
    return position == QStyleOptionViewItem::End || position == QStyleOptionViewItem::OnlyOne ||
           position == QStyleOptionViewItem::Invalid;
}

// The cell's place in its row as the style paints it: the first cell of a tree's row, after the indentation and the
// disclosure triangle, continues the row's selection rather than starting it.
int paintedPosition(const QStyleOptionViewItem &option)
{
    int position = option.viewItemPosition;
    if (option.features.testFlag(QStyleOptionViewItem::IsDecoratedRootColumn)) {
        if (position == QStyleOptionViewItem::Beginning) {
            position = QStyleOptionViewItem::Middle;
        } else if (position == QStyleOptionViewItem::OnlyOne || position == QStyleOptionViewItem::Invalid) {
            position = QStyleOptionViewItem::End;
        }
    }
    return position;
}

QColor selectionFill(const QStyleOption &option)
{
    const QPalette::ColorGroup group =
        option.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
    return option.palette.color(group, QPalette::Highlight);
}

QColor hairlineColor(const QPalette &palette)
{
    QColor line = palette.color(QPalette::Text);
    line.setAlphaF(0.1F);
    return line;
}

} // namespace

void paintRowSelection(QPainter *painter, const QRect &cell, int position, const QColor &fill)
{
    if (!painter || cell.isEmpty()) {
        return;
    }
    const bool first = startsRow(position);
    const bool last = endsRow(position);
    QRectF area(cell);
    area.adjust(first ? kRowInset : 0, 0, last ? -kRowInset : 0, 0);
    const Corners corners = first && last ? Corners::All : first ? Corners::Left : last ? Corners::Right : Corners::None;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->fillPath(squircle(area, kRadiusSmall, corners), fill);
    painter->restore();
}

namespace {

// Text stands kRowInset + kCellPadding from the view's sides, in line with the bars above the table, and kSpace2 from a
// neighbouring cell.
int leadingPadding(int position)
{
    return startsRow(position) ? kRowInset + kCellPadding : kSpace2;
}

int trailingPadding(int position)
{
    return endsRow(position) ? kRowInset + kCellPadding : kSpace2;
}

} // namespace

QRect cellContentRect(const QRect &cell, int position)
{
    return cell.adjusted(leadingPadding(position), 0, -trailingPadding(position), 0);
}

SelectaStyle::SelectaStyle()
    : QProxyStyle()
{}

void SelectaStyle::keepPlatformRows(QAbstractItemView *view)
{
    if (view) {
        view->setProperty(kPlatformRows, true);
    }
}

void SelectaStyle::hideHandles(QSplitter *splitter)
{
    if (splitter) {
        splitter->setProperty(kHiddenHandles, true);
    }
}

bool SelectaStyle::drawsRowsOf(const QWidget *widget)
{
    const auto *view = qobject_cast<const QAbstractItemView *>(widget);
    if (!view || qobject_cast<const QHeaderView *>(widget) || qobject_cast<const SourceListView *>(widget) ||
        view->property(kPlatformRows).toBool()) {
        return false;
    }
    // Menus and completion lists keep the platform's look.
    return view->window()->windowType() != Qt::Popup && !view->inherits("QComboBoxListView");
}

void SelectaStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (drawsRowsOf(widget)) {
        auto *view = qobject_cast<QAbstractItemView *>(widget);
        view->setAlternatingRowColors(false); // no stripes: the rows stand apart by their spacing
        view->setAttribute(Qt::WA_MacShowFocusRect, false);
        if (auto *frame = qobject_cast<QFrame *>(widget)) {
            frame->setFrameShape(QFrame::NoFrame);
        }
    } else if (auto *header = qobject_cast<QHeaderView *>(widget); header && drawsRowsOf(header->parentWidget())) {
        QFont font = header->font();
        font.setPointSizeF(font.pointSizeF() - 1.5);
        header->setFont(font);
        header->setHighlightSections(false);
    }
}

void SelectaStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                                 const QWidget *widget) const
{
    const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option);
    if (item && drawsRowsOf(widget)) {
        switch (element) {
        case PE_PanelItemViewItem:
            if (item->state.testFlag(State_Selected)) {
                paintRowSelection(painter, item->rect, paintedPosition(*item), selectionFill(*item));
            } else if (item->backgroundBrush.style() != Qt::NoBrush) {
                painter->fillRect(cellContentRect(item->rect, paintedPosition(*item))
                                      .adjusted(-kCellPadding, 0, kCellPadding, 0)
                                      .intersected(item->rect),
                                  item->backgroundBrush);
            }
            return;
        case PE_PanelItemViewRow:
            // A tree's indentation and disclosure triangle: the start of its row's selection. Elsewhere nothing, as
            // there are no stripes.
            if (item->features.testFlag(QStyleOptionViewItem::IsDecorationForRootColumn) &&
                item->state.testFlag(State_Selected) && item->rect.width() > kRowInset) {
                QRectF area(item->rect);
                area.setLeft(std::max<qreal>(area.left(), kRowInset));
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->fillPath(squircle(area, kRadiusSmall, Corners::Left), selectionFill(*item));
                painter->restore();
            }
            return;
        default:
            break;
        }
    }
    if (element == PE_FrameFocusRect && drawsRowsOf(widget)) {
        return; // the selection says where the keyboard is
    }
    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void SelectaStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                               const QWidget *widget) const
{
    if (element == CE_Splitter) {
        // A hairline between panes, as AppKit's thin dividers; nothing where the sidebar's glass is the gap.
        const QWidget *splitter = widget ? widget->parentWidget() : nullptr;
        if (!splitter || !splitter->property(kHiddenHandles).toBool()) {
            painter->fillRect(option->rect, hairlineColor(option->palette));
        }
        return;
    }
    const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option);
    const QWidget *view = widget ? widget->parentWidget() : nullptr;
    if (header && drawsRowsOf(view) && (element == CE_Header || element == CE_HeaderSection ||
                                        element == CE_HeaderEmptyArea || element == CE_HeaderLabel)) {
        const QPalette &palette = option->palette;
        if (element != CE_HeaderLabel) {
            // A quiet header: the table's own background and one hairline under it, no column separators.
            painter->fillRect(option->rect, palette.color(QPalette::Base));
            painter->fillRect(QRect(option->rect.left(), option->rect.bottom(), option->rect.width(), 1),
                              hairlineColor(palette));
        }
        if (element == CE_Header || element == CE_HeaderLabel) {
            const bool first = header->position == QStyleOptionHeader::Beginning ||
                               header->position == QStyleOptionHeader::OnlyOneSection;
            const bool last = header->position == QStyleOptionHeader::End ||
                              header->position == QStyleOptionHeader::OnlyOneSection;
            const int left = first ? kRowInset + kCellPadding : kSpace2;
            const int right = last ? kRowInset + kCellPadding : kSpace2;
            QRect text = option->rect.adjusted(left, 0, -right, -1);
            if (header->sortIndicator != QStyleOptionHeader::None) {
                QStyleOptionHeader arrow = *header;
                constexpr int kArrow = 8;
                const bool rightAligned = header->textAlignment.testFlag(Qt::AlignRight);
                arrow.rect = rightAligned ? QRect(text.left(), text.center().y() - (kArrow / 2), kArrow, kArrow)
                                          : QRect(text.right() - kArrow, text.center().y() - (kArrow / 2), kArrow,
                                                  kArrow);
                drawPrimitive(PE_IndicatorHeaderArrow, &arrow, painter, widget);
                text.adjust(rightAligned ? kArrow + kSpace1 : 0, 0, rightAligned ? 0 : -(kArrow + kSpace1), 0);
            }
            painter->save();
            painter->setPen(palette.color(QPalette::PlaceholderText));
            const QString label = painter->fontMetrics().elidedText(header->text, Qt::ElideRight, text.width());
            painter->drawText(text, static_cast<int>(header->textAlignment | Qt::AlignVCenter) | Qt::TextSingleLine,
                              label);
            painter->restore();
        }
        return;
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

QRect SelectaStyle::subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const
{
    const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option);
    if (item && drawsRowsOf(widget) &&
        (element == SE_ItemViewItemText || element == SE_ItemViewItemDecoration ||
         element == SE_ItemViewItemCheckIndicator || element == SE_ItemViewItemFocusRect)) {
        // The base style keeps its own margin inside the rectangle it gets: give it the content rectangle less that.
        const int margin = QProxyStyle::pixelMetric(PM_FocusFrameHMargin, option, widget) + 1;
        QStyleOptionViewItem inset = *item;
        inset.rect = cellContentRect(item->rect, paintedPosition(*item)).adjusted(-margin, 0, margin, 0);
        return QProxyStyle::subElementRect(element, &inset, widget);
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

QSize SelectaStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                                     const QWidget *widget) const
{
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    if (type == CT_ItemViewItem && drawsRowsOf(widget)) {
        const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option);
        const int position = item ? paintedPosition(*item) : QStyleOptionViewItem::Middle;
        const int margin = 2 * (QProxyStyle::pixelMetric(PM_FocusFrameHMargin, option, widget) + 1); // already in
        result.setWidth(result.width() - margin + leadingPadding(position) + trailingPadding(position));
        result.setHeight(std::max(result.height(), kRowHeight));
    } else if (type == CT_HeaderSection && widget && drawsRowsOf(widget->parentWidget())) {
        result.setHeight(kRowHeight);
        result.setWidth(result.width() + (2 * kCellPadding));
    }
    return result;
}

int SelectaStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                            QStyleHintReturn *returnData) const
{
    if (hint == SH_ItemView_ShowDecorationSelected && drawsRowsOf(widget)) {
        return 1; // the selection runs under the disclosure triangle too, as one row
    }
    if (hint == SH_ItemView_PaintAlternatingRowColorsForEmptyArea && drawsRowsOf(widget)) {
        return 0;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace selecta::app::style
