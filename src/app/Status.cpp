// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Status.h"

#include "app/Appearance.h"
#include "app/Style.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <array>
#include <cmath>
#include <numbers>

using namespace Qt::StringLiterals;

namespace selecta::app::status {
namespace {

// The macOS system colours (NSColor.system*), light then dark, for when AppKit can't give them (and off macOS).
struct Pair
{
    QRgb light;
    QRgb dark;
};

Pair fallback(Kind kind)
{
    switch (kind) {
    case Kind::Waiting:
        return {0xFF8E8E93, 0xFF98989D};
    case Kind::Working:
        return {0xFF007AFF, 0xFF0A84FF};
    case Kind::Release:
        return {0xFF5856D6, 0xFF6D6BF0};
    case Kind::Ready:
        return {0xFF28CD41, 0xFF32D74B};
    case Kind::Check:
        return {0xFFFF9500, 0xFFFF9F0A};
    case Kind::Failed:
        return {0xFFFF3B30, 0xFFFF453A};
    case Kind::Have:
        return {0xFFAF52DE, 0xFFBF5AF2};
    case Kind::Inbox:
        return {0xFF1BA3E8, 0xFF5AC8F5};
    case Kind::Upgrade:
        return {0xFF30A8B8, 0xFF5AC4D6};
    }
    return {0xFF8E8E93, 0xFF98989D};
}

// ---- Selecta's drawings of the SF Symbols it uses, on a 20 × 20 grid (as mock/index.html draws them) ----

constexpr qreal kGrid = 20.0;
constexpr qreal kStroke = 1.6;
constexpr qreal kKnock = 1.8; // the cut-out glyph of a filled symbol

QPen pen(const QColor &color, qreal width)
{
    QPen p(color, width);
    p.setCapStyle(Qt::RoundCap);
    p.setJoinStyle(Qt::RoundJoin);
    return p;
}

QPainterPath circle(qreal radius, QPointF centre = {10, 10})
{
    QPainterPath path;
    path.addEllipse(centre, radius, radius);
    return path;
}

QPainterPath polyline(std::initializer_list<QPointF> points)
{
    QPainterPath path;
    bool first = true;
    for (const QPointF point : points) {
        if (first) {
            path.moveTo(point);
            first = false;
        } else {
            path.lineTo(point);
        }
    }
    return path;
}

QPainterPath seal()
{
    // A wavy circle, twelve bumps.
    constexpr int kSteps = 120;
    constexpr int kBumps = 12;
    QPainterPath path;
    for (int i = 0; i < kSteps; ++i) {
        const double angle = 2.0 * std::numbers::pi * i / kSteps;
        const double radius = 7.7 + (0.6 * std::cos(kBumps * angle));
        const QPointF point(10.0 + (radius * std::cos(angle)), 10.0 + (radius * std::sin(angle)));
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    path.closeSubpath();
    return path;
}

// Fills `shape`, then cuts `glyph` (stroked) and `dots` (filled) out of it.
void drawFilled(QPainter &painter, const QPainterPath &shape, const QPainterPath &glyph, const QColor &color,
                const QPainterPath &dots = {}, qreal glyphWidth = kKnock)
{
    painter.fillPath(shape, color);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.strokePath(glyph, pen(Qt::black, glyphWidth));
    painter.fillPath(dots, Qt::black);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

bool drawSymbol(QPainter &painter, const QString &name, const QColor &color)
{
    const QPen line = pen(color, kStroke);
    if (name == u"clock") {
        painter.strokePath(circle(7.6), line);
        painter.strokePath(polyline({{10, 5.8}, {10, 10}, {12.8, 11.8}}), line);
    } else if (name == u"hourglass") {
        QPainterPath path = polyline({{5.6, 2.8}, {14.4, 2.8}});
        path.addPath(polyline({{5.6, 17.2}, {14.4, 17.2}}));
        path.moveTo(6.6, 2.8);
        path.cubicTo(6.6, 6.4, 13.4, 6.7, 13.4, 10);
        path.cubicTo(13.4, 13.3, 6.6, 13.6, 6.6, 17.2);
        path.moveTo(13.4, 2.8);
        path.cubicTo(13.4, 6.4, 6.6, 6.7, 6.6, 10);
        path.cubicTo(6.6, 13.3, 13.4, 13.6, 13.4, 17.2);
        painter.strokePath(path, line);
    } else if (name == u"magnifyingglass") {
        painter.strokePath(circle(5.6, {8.6, 8.6}), line);
        painter.strokePath(polyline({{12.8, 12.8}, {17, 17}}), pen(color, 2.0));
    } else if (name == u"opticaldisc.fill") {
        QPainterPath glyph = circle(2.1);
        glyph.arcMoveTo(QRectF(4.6, 4.6, 10.8, 10.8), 90);
        glyph.arcTo(QRectF(4.6, 4.6, 10.8, 10.8), 90, -90);
        drawFilled(painter, circle(8.3), glyph, color, {}, 1.4);
    } else if (name == u"checkmark.circle.fill") {
        drawFilled(painter, circle(8.3), polyline({{6.5, 10.4}, {8.9, 12.8}, {13.6, 7.6}}), color);
    } else if (name == u"checkmark.seal.fill") {
        drawFilled(painter, seal(), polyline({{6.7, 10.3}, {8.9, 12.5}, {13.3, 7.7}}), color);
    } else if (name == u"tray.full.fill") {
        QPainterPath tray;
        tray.moveTo(2.6, 11.5);
        tray.lineTo(4.4, 5.3);
        tray.cubicTo(4.6, 4.6, 5.2, 4.2, 5.9, 4.2);
        tray.lineTo(14.1, 4.2);
        tray.cubicTo(14.8, 4.2, 15.4, 4.6, 15.6, 5.3);
        tray.lineTo(17.4, 11.5);
        tray.lineTo(17.4, 15.2);
        tray.cubicTo(17.4, 16.1, 16.7, 16.8, 15.8, 16.8);
        tray.lineTo(4.2, 16.8);
        tray.cubicTo(3.3, 16.8, 2.6, 16.1, 2.6, 15.2);
        tray.closeSubpath();
        QPainterPath glyph = polyline({{3, 11.5}, {6.9, 11.5}, {8, 13.3}, {12, 13.3}, {13.1, 11.5}, {17, 11.5}});
        glyph.addPath(polyline({{6.9, 7}, {13.1, 7}}));
        glyph.addPath(polyline({{6.4, 9.2}, {13.6, 9.2}}));
        drawFilled(painter, tray, glyph, color, {}, 1.3);
    } else if (name == u"exclamationmark.triangle.fill") {
        QPainterPath triangle = polyline({{10, 2.8}, {17.6, 16.4}, {2.4, 16.4}});
        triangle.closeSubpath();
        painter.strokePath(triangle, pen(color, 1.8)); // rounds the corners
        QPainterPath dot = circle(1.0, {10, 13.9});
        drawFilled(painter, triangle, polyline({{10, 7.4}, {10, 11.1}}), color, dot);
    } else if (name == u"xmark.circle.fill") {
        QPainterPath glyph = polyline({{7.3, 7.3}, {12.7, 12.7}});
        glyph.addPath(polyline({{12.7, 7.3}, {7.3, 12.7}}));
        drawFilled(painter, circle(8.3), glyph, color);
    } else if (name == u"exclamationmark.octagon.fill") {
        QPainterPath octagon =
            polyline({{7.1, 2.4}, {12.9, 2.4}, {17.6, 7.1}, {17.6, 12.9}, {12.9, 17.6}, {7.1, 17.6}, {2.4, 12.9},
                      {2.4, 7.1}});
        octagon.closeSubpath();
        painter.strokePath(octagon, pen(color, 1.0));
        drawFilled(painter, octagon, polyline({{10, 6.2}, {10, 10.8}}), color, circle(1.0, {10, 13.7}));
    } else if (name == u"arrow.down.circle.fill") {
        QPainterPath glyph = polyline({{10, 5.6}, {10, 13.6}});
        glyph.addPath(polyline({{6.9, 10.6}, {10, 13.7}, {13.1, 10.6}}));
        drawFilled(painter, circle(8.3), glyph, color);
    } else if (name == u"arrow.up.circle.fill") {
        QPainterPath glyph = polyline({{10, 14.4}, {10, 6.4}});
        glyph.addPath(polyline({{6.9, 9.4}, {10, 6.3}, {13.1, 9.4}}));
        drawFilled(painter, circle(8.3), glyph, color);
    } else if (name == u"arrow.triangle.2.circlepath") {
        const QRectF ring(4, 4, 12, 12);
        QPainterPath path;
        path.arcMoveTo(ring, 174.3);
        path.arcTo(ring, 174.3, -131.9);
        path.addPath(polyline({{15.4, 2.8}, {15.4, 6.2}, {12, 6.2}}));
        path.arcMoveTo(ring, -5.7);
        path.arcTo(ring, -5.7, -131.9);
        path.addPath(polyline({{4.6, 17.2}, {4.6, 13.8}, {8, 13.8}}));
        painter.strokePath(path, line);
    } else if (name == u"questionmark.circle.fill") {
        QPainterPath glyph;
        glyph.moveTo(7.7, 7.7);
        glyph.cubicTo(8.1, 6.4, 9.4, 5.9, 10.4, 6.1);
        glyph.cubicTo(11.5, 6.3, 12.3, 7.3, 12.3, 8.6);
        glyph.cubicTo(12.3, 10.2, 10, 10.5, 10, 11.9);
        drawFilled(painter, circle(8.3), glyph, color, circle(1.0, {10, 14.3}), 1.6);
    } else if (name == u"minus.circle.fill") {
        drawFilled(painter, circle(8.3), polyline({{6.4, 10}, {13.6, 10}}), color);
    } else if (name == u"trash") {
        QPainterPath path = polyline({{3.6, 5.4}, {16.4, 5.4}});
        path.addPath(polyline({{8, 5.4}, {8, 3.8}, {12, 3.8}, {12, 5.4}}));
        path.moveTo(5.4, 5.4);
        path.lineTo(6.2, 16);
        path.cubicTo(6.3, 16.7, 6.8, 17.2, 7.5, 17.2);
        path.lineTo(12.5, 17.2);
        path.cubicTo(13.2, 17.2, 13.7, 16.7, 13.8, 16);
        path.lineTo(14.6, 5.4);
        painter.strokePath(path, line);
    } else {
        return false;
    }
    return true;
}

} // namespace

QColor color(Kind kind, bool dark)
{
#ifdef Q_OS_MACOS
    if (const QColor system = macStatusColor(static_cast<int>(kind), dark); system.isValid()) {
        return system;
    }
#endif
    const Pair pair = fallback(kind);
    return QColor::fromRgba(dark ? pair.dark : pair.light);
}

QColor color(Kind kind, const QPalette &palette)
{
    return color(kind, isDarkPalette(palette));
}

QPixmap drawnPixmap(const QString &symbol, const QColor &color, int size, qreal devicePixelRatio)
{
    if (size <= 0 || devicePixelRatio <= 0.0) {
        return {};
    }
    const int pixels = static_cast<int>(std::lround(size * devicePixelRatio));
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(pixels / kGrid, pixels / kGrid);
        if (!drawSymbol(painter, symbol, color)) {
            return {};
        }
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    return pixmap;
}

QIcon icon(const QString &symbol)
{
#ifdef Q_OS_MACOS
    if (QIcon system = QIcon::fromTheme(symbol); !system.isNull()) {
        return system;
    }
#endif
    constexpr int kLarge = 64; // QIcon scales it down for smaller sizes
    const QPixmap drawn = drawnPixmap(symbol, Qt::black, kLarge, 1.0);
    return drawn.isNull() ? QIcon() : QIcon(drawn);
}

QPixmap pixmap(const QString &symbol, const QColor &color, int size, qreal devicePixelRatio)
{
#ifdef Q_OS_MACOS
    if (const QIcon system = QIcon::fromTheme(symbol); !system.isNull()) {
        const QPixmap source = system.pixmap(QSize(size, size), devicePixelRatio);
        if (!source.isNull()) {
            QPixmap tinted(source.size());
            tinted.setDevicePixelRatio(source.devicePixelRatio());
            tinted.fill(Qt::transparent);
            QPainter painter(&tinted);
            painter.drawPixmap(0, 0, source);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn); // keeps the cut-out glyph
            painter.fillRect(QRectF(QPointF(0, 0), tinted.deviceIndependentSize()), color);
            return tinted;
        }
    }
#endif
    return drawnPixmap(symbol, color, size, devicePixelRatio);
}

StatusDelegate::StatusDelegate(QObject *parent, bool symbolOnly)
    : QStyledItemDelegate(parent)
    , m_symbolOnly(symbolOnly)
{}

void StatusDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QString symbol = index.data(kSymbolRole).toString();
    if (symbol.isEmpty() || !painter) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString text = opt.text;
    // The cell's background and the row's selection, as the style paints them; the content is ours.
    opt.text.clear();
    opt.icon = QIcon();
    opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const bool selected = opt.state.testFlag(QStyle::State_Selected);
    const auto kind = static_cast<Kind>(index.data(kKindRole).toInt());
    const QPalette &palette = opt.palette;
    const QColor tint = selected ? palette.color(QPalette::HighlightedText) : color(kind, palette);
    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : qApp->devicePixelRatio();
    const QPixmap glyph = pixmap(symbol, tint, kSymbolSize, dpr);
    const QRect content = style::cellContentRect(option.rect, option.viewItemPosition);

    painter->save();
    if (m_symbolOnly) {
        const QPointF at(content.center().x() - (kSymbolSize / 2.0) + 0.5, content.center().y() - (kSymbolSize / 2.0) + 0.5);
        painter->drawPixmap(at, glyph);
        painter->restore();
        return;
    }
    const int top = content.top() + ((content.height() - kSymbolSize) / 2);
    painter->drawPixmap(QPoint(content.left(), top), glyph);
    const QRect textRect = content.adjusted(kSymbolSize + style::kSpace1 + 2, 0, 0, 0);
    QColor ink = palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    if (kind == Kind::Waiting && !selected) {
        ink = palette.color(QPalette::PlaceholderText); // waiting is the quiet one
    }
    painter->setFont(opt.font);
    painter->setPen(ink);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                      opt.fontMetrics.elidedText(text, Qt::ElideRight, textRect.width()));
    painter->restore();
}

QSize StatusDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(kSymbolRole).toString().isEmpty()) {
        return size;
    }
    const QRect padding = style::cellContentRect(QRect(0, 0, 100, 1), option.viewItemPosition);
    const int pad = 100 - padding.width();
    if (m_symbolOnly) {
        size.setWidth(kSymbolSize + pad);
    } else {
        const int textWidth = option.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString());
        size.setWidth(kSymbolSize + style::kSpace1 + 2 + textWidth + pad);
    }
    size.setHeight(std::max(size.height(), style::kRowHeight));
    return size;
}

bool StatusDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    if (m_symbolOnly && event && view && event->type() == QEvent::ToolTip &&
        index.data(Qt::ToolTipRole).toString().isEmpty()) {
        // The words of a symbol shown alone (the search results' status, P-003).
        const QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            QToolTip::showText(event->globalPos(), text, view, option.rect);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

} // namespace selecta::app::status
