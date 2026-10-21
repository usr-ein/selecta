// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Status.h"

#include <QImage>
#include <QPainter>
#include <QSet>
#include <QStandardItemModel>
#include <QTreeView>

#include <gtest/gtest.h>

using namespace selecta::app::status;

namespace {

int opaquePixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y).alpha() > 128 ? 1 : 0;
        }
    }
    return count;
}

constexpr std::array kKinds{Kind::Waiting, Kind::Working, Kind::Release, Kind::Ready,  Kind::Check,
                            Kind::Failed,  Kind::Have,    Kind::Inbox,   Kind::Upgrade};

} // namespace

TEST(StatusTest, EverySymbolHasADrawingAndAnIcon)
{
    for (const char *name : kSymbols) {
        const QString symbol = QString::fromLatin1(name);
        const QPixmap drawn = drawnPixmap(symbol, Qt::black, 20, 2.0);
        ASSERT_FALSE(drawn.isNull()) << name;
        EXPECT_EQ(drawn.deviceIndependentSize(), QSizeF(20, 20)) << name;
        EXPECT_GT(opaquePixels(drawn.toImage()), 20) << name;
        EXPECT_FALSE(icon(symbol).isNull()) << name;
        EXPECT_FALSE(pixmap(symbol, Qt::red, 15, 2.0).isNull()) << name;
    }
    EXPECT_TRUE(drawnPixmap(QStringLiteral("not.a.symbol"), Qt::black, 20, 1.0).isNull());
    EXPECT_TRUE(drawnPixmap(QStringLiteral("clock"), Qt::black, 0, 1.0).isNull());
}

TEST(StatusTest, AFilledSymbolKeepsItsGlyphCutOut)
{
    // checkmark.circle.fill: the disc in the colour, the tick transparent, so a selection shows through it.
    const QImage image = drawnPixmap(QStringLiteral("checkmark.circle.fill"), Qt::red, 20, 4.0).toImage();
    const auto at = [&](qreal x, qreal y) { return image.pixelColor(QPointF(x * 4, y * 4).toPoint()); };
    EXPECT_EQ(at(10, 4).alpha(), 255);   // inside the disc
    EXPECT_EQ(at(10, 4).red(), 255);     // in its colour
    EXPECT_EQ(at(8.9, 12.8).alpha(), 0); // on the tick
    EXPECT_EQ(at(0.5, 0.5).alpha(), 0);  // outside
}

TEST(StatusTest, EachKindHasItsOwnColourInEachAppearance)
{
    QSet<QRgb> light;
    QSet<QRgb> dark;
    for (const Kind kind : kKinds) {
        const QColor l = color(kind, false);
        const QColor d = color(kind, true);
        ASSERT_TRUE(l.isValid());
        ASSERT_TRUE(d.isValid());
        light.insert(l.rgb());
        dark.insert(d.rgb());
    }
    EXPECT_EQ(light.size(), static_cast<qsizetype>(kKinds.size()));
    EXPECT_EQ(dark.size(), static_cast<qsizetype>(kKinds.size()));
    // Waiting is the quiet one: grey. The others are colours.
    EXPECT_LT(color(Kind::Waiting, false).hsvSaturation(), 40);
    for (const Kind kind : kKinds) {
        if (kind != Kind::Waiting) {
            EXPECT_GT(color(kind, false).hsvSaturation(), 100) << static_cast<int>(kind);
        }
    }
    // The colours read as the kinds: green ready, red failed, orange check.
    EXPECT_LT(std::abs(color(Kind::Ready, false).hue() - 130), 25);
    EXPECT_TRUE(color(Kind::Failed, false).hue() < 15 || color(Kind::Failed, false).hue() > 345);
    EXPECT_LT(std::abs(color(Kind::Check, false).hue() - 35), 15);
}

namespace {

struct Painted
{
    QImage image;
    QRect cell;
};

Painted paintCell(bool symbolOnly, bool selected)
{
    QStandardItemModel model(1, 1);
    auto *item = new QStandardItem(QStringLiteral("Ready"));
    item->setData(QStringLiteral("checkmark.circle.fill"), kSymbolRole);
    item->setData(static_cast<int>(Kind::Ready), kKindRole);
    model.setItem(0, 0, item);
    QTreeView view;
    view.setModel(&model);
    view.setRootIsDecorated(false);
    view.setHeaderHidden(true);
    view.setItemDelegate(new StatusDelegate(&view, symbolOnly));
    view.resize(200, 60);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.show();
    if (selected) {
        view.selectionModel()->select(model.index(0, 0), QItemSelectionModel::ClearAndSelect);
    }
    return {view.viewport()->grab().toImage(), view.visualRect(model.index(0, 0))};
}

int pixelsNear(const QImage &image, QRect area, const QColor &colour)
{
    int count = 0;
    const qreal dpr = image.devicePixelRatio();
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor pixel = image.pixelColor(QPoint(x, y) * dpr);
            count += std::abs(pixel.red() - colour.red()) + std::abs(pixel.green() - colour.green()) +
                                 std::abs(pixel.blue() - colour.blue()) <
                             60
                         ? 1
                         : 0;
        }
    }
    return count;
}

} // namespace

TEST(StatusTest, TheDelegateTintsTheSymbolWithItsKind)
{
    const Painted painted = paintCell(false, false);
    const QColor green = color(Kind::Ready, QTreeView().palette());
    // The symbol at the cell's start, after its padding, in green.
    const QRect symbolArea(painted.cell.left() + 18, painted.cell.top(), 20, painted.cell.height());
    EXPECT_GT(pixelsNear(painted.image, symbolArea, green), 10);
    // Then the words: dark pixels further right, none of them green.
    const QRect textArea(painted.cell.left() + 42, painted.cell.top(), 60, painted.cell.height());
    EXPECT_EQ(pixelsNear(painted.image, textArea, green), 0);
}

TEST(StatusTest, ASymbolAloneSitsInTheMiddleOfItsCell)
{
    const Painted painted = paintCell(true, false);
    const QColor green = color(Kind::Ready, QTreeView().palette());
    const int middle = painted.cell.center().x();
    EXPECT_GT(pixelsNear(painted.image, QRect(middle - 8, painted.cell.top(), 16, painted.cell.height()), green), 10);
    EXPECT_EQ(
        pixelsNear(painted.image, QRect(painted.cell.left(), painted.cell.top(), 40, painted.cell.height()), green), 0);
}

TEST(StatusTest, InASelectedRowTheSymbolTakesTheSelectionsTextColour)
{
    const Painted painted = paintCell(false, true);
    const QColor green = color(Kind::Ready, QTreeView().palette());
    const QRect symbolArea(painted.cell.left() + 18, painted.cell.top(), 20, painted.cell.height());
    EXPECT_EQ(pixelsNear(painted.image, symbolArea, green), 0);
}
