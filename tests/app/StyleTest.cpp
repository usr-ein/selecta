// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/SourceListView.h"
#include "app/Style.h"

#include <QHeaderView>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTreeView>

#include <gtest/gtest.h>

using namespace selecta::app::style;

namespace {

bool insideOf(const QPainterPath &path, qreal x, qreal y)
{
    return path.contains(QPointF(x, y));
}

// How far down from the top the shape starts, at this x.
qreal topEdge(const QPainterPath &path, qreal x)
{
    for (qreal y = 0; y < path.boundingRect().height(); y += 0.05) {
        if (path.contains(QPointF(x, y))) {
            return y;
        }
    }
    return -1;
}

} // namespace

TEST(StyleTest, SpacingIsOneScale)
{
    // The mock's scale (mock/styles.css --sp-1 … --sp-6), and what is built on it.
    EXPECT_EQ(kSpace1, 4);
    EXPECT_EQ(kSpace2, 8);
    EXPECT_EQ(kSpace3, 12);
    EXPECT_EQ(kSpace4, 16);
    EXPECT_EQ(kSpace5, 20);
    EXPECT_EQ(kSpace6, 28);
    EXPECT_EQ(kRowHeight, 28);
    EXPECT_EQ(kTwoLineRowHeight, 44);
    EXPECT_EQ(kRowInset + kCellPadding, kSpace5); // text in a table lines up with the bars' 20 px margin
}

TEST(StyleTest, SquircleCornersAreContinuousNotCircular)
{
    const QRectF rect(0, 0, 400, 300);
    constexpr qreal kRadius = 100;
    const QPainterPath path = squircle(rect, kRadius);
    EXPECT_EQ(path.boundingRect(), rect);
    EXPECT_TRUE(insideOf(path, 200, 150));
    EXPECT_FALSE(insideOf(path, 3, 3)); // the corner itself is cut
    QPainterPath circular;
    circular.addRoundedRect(rect, kRadius, kRadius);
    // Both pass through about the same point at 45° (0.29 radii from each side)...
    EXPECT_NEAR(topEdge(path, 0.293 * kRadius), topEdge(circular, 0.293 * kRadius), 1.5);
    // ...but the continuous corner eases into the side: 0.8 radii along, a circle is almost back on the side and the
    // continuous curve still well inside it; it only meets the side about 1.53 radii along.
    EXPECT_GT(topEdge(path, 0.8 * kRadius), topEdge(circular, 0.8 * kRadius) + 0.8);
    EXPECT_GT(topEdge(path, 1.2 * kRadius), 0.0);
    EXPECT_NEAR(topEdge(circular, 1.2 * kRadius), 0.0, 0.06);
    EXPECT_NEAR(topEdge(path, 1.6 * kRadius), 0.0, 0.06); // past the curve, the side is straight
    // Symmetric about both axes.
    for (const QPointF point : {QPointF(5, 9), QPointF(14, 3), QPointF(2, 30), QPointF(25, 1), QPointF(80, 3)}) {
        const bool in = insideOf(path, point.x(), point.y());
        EXPECT_EQ(insideOf(path, rect.width() - point.x(), point.y()), in);
        EXPECT_EQ(insideOf(path, point.x(), rect.height() - point.y()), in);
        EXPECT_EQ(insideOf(path, rect.width() - point.x(), rect.height() - point.y()), in);
    }
}

TEST(StyleTest, SquircleEdgeCases)
{
    EXPECT_TRUE(squircle(QRectF(), 10).isEmpty());
    EXPECT_TRUE(squircle(QRectF(0, 0, 0, 20), 10).isEmpty());
    // No radius: the rectangle.
    const QPainterPath square = squircle(QRectF(0, 0, 40, 20), 0);
    EXPECT_TRUE(insideOf(square, 0.5, 0.5));
    // Half the height or more: a capsule, with round ends.
    const QPainterPath capsule = squircle(QRectF(0, 0, 100, 28), 14);
    EXPECT_EQ(capsule.boundingRect(), QRectF(0, 0, 100, 28));
    EXPECT_TRUE(insideOf(capsule, 0.5, 14));
    EXPECT_FALSE(insideOf(capsule, 2, 2));
    EXPECT_TRUE(insideOf(capsule, 50, 0.5));
    // A radius too big for the continuous curve gives way: the path stays within its rectangle.
    const QPainterPath small = squircle(QRectF(10, 10, 30, 12), 5);
    EXPECT_EQ(small.boundingRect(), QRectF(10, 10, 30, 12));
    EXPECT_TRUE(insideOf(small, 25, 16));
}

TEST(StyleTest, SquircleCanRoundOneSide)
{
    const QRectF rect(0, 0, 100, 30);
    const QPainterPath left = squircle(rect, 6, Corners::Left);
    EXPECT_FALSE(insideOf(left, 0.5, 0.5));
    EXPECT_TRUE(insideOf(left, 99.5, 0.5)); // square on the right
    const QPainterPath right = squircle(rect, 6, Corners::Right);
    EXPECT_TRUE(insideOf(right, 0.5, 0.5));
    EXPECT_FALSE(insideOf(right, 99.5, 0.5));
    const QPainterPath none = squircle(rect, 6, Corners::None);
    EXPECT_TRUE(insideOf(none, 0.5, 0.5));
    EXPECT_TRUE(insideOf(none, 99.5, 29.5));
}

TEST(StyleTest, CellsHaveRoomAtTheRowsEnds)
{
    const QRect cell(0, 0, 200, 28);
    EXPECT_EQ(cellContentRect(cell, QStyleOptionViewItem::Beginning), QRect(20, 0, 172, 28));
    EXPECT_EQ(cellContentRect(cell, QStyleOptionViewItem::Middle), QRect(8, 0, 184, 28));
    EXPECT_EQ(cellContentRect(cell, QStyleOptionViewItem::End), QRect(8, 0, 172, 28));
    EXPECT_EQ(cellContentRect(cell, QStyleOptionViewItem::OnlyOne), QRect(20, 0, 160, 28));
    EXPECT_EQ(cellContentRect(cell, QStyleOptionViewItem::Invalid), QRect(20, 0, 160, 28)); // a list's only cell
}

TEST(StyleTest, ASelectedRowIsOneInsetSquircleAcrossItsCells)
{
    QImage image(300, 28, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        paintRowSelection(&painter, QRect(0, 0, 100, 28), QStyleOptionViewItem::Beginning, Qt::blue);
        paintRowSelection(&painter, QRect(100, 0, 100, 28), QStyleOptionViewItem::Middle, Qt::blue);
        paintRowSelection(&painter, QRect(200, 0, 100, 28), QStyleOptionViewItem::End, Qt::blue);
    }
    EXPECT_EQ(image.pixelColor(3, 14), QColor(Qt::white));   // the inset on the left
    EXPECT_EQ(image.pixelColor(296, 14), QColor(Qt::white)); // and on the right
    EXPECT_EQ(image.pixelColor(8, 0), QColor(Qt::white));    // rounded corners
    EXPECT_EQ(image.pixelColor(291, 27), QColor(Qt::white));
    for (const int x : {20, 99, 100, 150, 199, 200, 280}) { // continuous through the cells' joints
        EXPECT_EQ(image.pixelColor(x, 14), QColor(Qt::blue)) << x;
        EXPECT_EQ(image.pixelColor(x, 0), QColor(Qt::blue)) << x;
    }
}

TEST(StyleTest, TheStyleDrawsTablesNotTheSourceListOrMenus)
{
    QTreeView tree;
    selecta::app::SourceListView sources;
    QTreeView kept;
    SelectaStyle::keepPlatformRows(&kept);
    EXPECT_TRUE(SelectaStyle::drawsRowsOf(&tree));
    EXPECT_FALSE(SelectaStyle::drawsRowsOf(&sources));
    EXPECT_FALSE(SelectaStyle::drawsRowsOf(&kept));
    EXPECT_FALSE(SelectaStyle::drawsRowsOf(nullptr));
    EXPECT_FALSE(SelectaStyle::drawsRowsOf(tree.header())); // not a view of rows
    QMenu popup;
    QTreeView inPopup(&popup);
    EXPECT_FALSE(SelectaStyle::drawsRowsOf(&inPopup));
}

TEST(StyleTest, ATableHasNoStripesTallRowsAndAQuietHeader)
{
    SelectaStyle style;
    QStandardItemModel model(3, 2);
    model.setHorizontalHeaderLabels({QStringLiteral("Track"), QStringLiteral("Status")});
    for (int row = 0; row < 3; ++row) {
        model.setItem(row, 0, new QStandardItem(QStringLiteral("Artist - Title %1").arg(row)));
        model.setItem(row, 1, new QStandardItem(QStringLiteral("Ready")));
    }
    QTreeView view;
    view.setStyle(&style);
    view.header()->setStyle(&style);
    view.setAlternatingRowColors(true);
    view.setRootIsDecorated(false);
    view.setModel(&model);
    view.resize(400, 200);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.show();
    style.polish(&view);
    style.polish(view.header());
    EXPECT_FALSE(view.alternatingRowColors());
    EXPECT_GE(view.visualRect(model.index(0, 0)).height(), kRowHeight);
    EXPECT_EQ(view.header()->sizeHint().height(), kRowHeight);

    // A selected row: inset from both sides, filled in between.
    view.selectionModel()->select(model.index(1, 0),
                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const QImage image = view.viewport()->grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    const QRect row = view.visualRect(model.index(1, 0)).united(view.visualRect(model.index(1, 1)));
    const QColor base = view.palette().color(QPalette::Base);
    const auto at = [&](int x, int y) { return image.pixelColor(QPoint(x, y) * dpr); };
    EXPECT_EQ(at(row.left() + 2, row.center().y()), base);
    EXPECT_EQ(at(view.viewport()->width() - 3, row.center().y()), base);
    EXPECT_NE(at(row.left() + 40, row.top() + 3), base);
    EXPECT_NE(at(view.viewport()->width() / 2, row.top() + 3), base);
}
