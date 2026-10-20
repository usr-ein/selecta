// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/TickDelegate.h"

#include <QStandardItemModel>
#include <QTest>
#include <QTreeView>

#include <gtest/gtest.h>

using selecta::app::TickDelegate;

TEST(TickDelegateTest, AClickTogglesACheckableCell)
{
    QStandardItemModel model(2, 1);
    auto *checkable = new QStandardItem;
    checkable->setCheckable(true);
    checkable->setCheckState(Qt::Unchecked);
    model.setItem(0, 0, checkable);
    model.setItem(1, 0, new QStandardItem(QStringLiteral("not checkable")));

    QTreeView view;
    view.setModel(&model);
    view.setItemDelegateForColumn(0, new TickDelegate(&view));
    view.resize(200, 100);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.show();

    const QPoint first = view.visualRect(model.index(0, 0)).center();
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, first);
    EXPECT_EQ(checkable->checkState(), Qt::Checked);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, first);
    EXPECT_EQ(checkable->checkState(), Qt::Unchecked);

    // A cell without a check state stays as it is.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, view.visualRect(model.index(1, 0)).center());
    EXPECT_FALSE(model.item(1, 0)->data(Qt::CheckStateRole).isValid());
}

TEST(TickDelegateTest, PaintsWithoutTheStylesIndicator)
{
    QStandardItemModel model(1, 1);
    auto *item = new QStandardItem;
    item->setCheckable(true);
    item->setCheckState(Qt::Checked);
    model.setItem(0, 0, item);
    QTreeView view;
    view.setModel(&model);
    view.setItemDelegateForColumn(0, new TickDelegate(&view));
    view.resize(120, 60);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.show();
    const QImage image = view.viewport()->grab().toImage();
    const QRect cell = view.visualRect(model.index(0, 0));
    // The accent-coloured box sits in the middle of the cell.
    const QColor middle = image.pixelColor(cell.center() * image.devicePixelRatio() + QPoint(-4, 0));
    const QColor accent = view.palette().color(QPalette::Accent);
    EXPECT_LT(std::abs(middle.hue() - accent.hue()), 30) << middle.name().toStdString();
}
