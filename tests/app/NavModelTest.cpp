// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/NavModel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::NavModel;

TEST(NavModel, PassesQtModelChecks)
{
    NavModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCount(u"inbox"_s, 3);
    model.setCount(u"inbox"_s, 0);
}

TEST(NavModel, MainFlowComesFirst)
{
    const NavModel model;
    const QStringList ids = model.ids();
    ASSERT_GE(ids.size(), 2);
    EXPECT_EQ(ids.at(0), u"get"_s); // PRD D27: Get tracks is the home screen
    EXPECT_EQ(ids.at(1), u"inbox"_s);
    EXPECT_EQ(QSet<QString>(ids.begin(), ids.end()).size(), ids.size()); // ids are unique
}

TEST(NavModel, HeadersAreNotSelectable)
{
    const NavModel model;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row);
        const bool isHeader = index.data(NavModel::IsHeaderRole).toBool();
        EXPECT_EQ(model.isNavigable(row), !isHeader);
        EXPECT_EQ(index.flags().testFlag(Qt::ItemIsSelectable), !isHeader);
        EXPECT_EQ(index.data(NavModel::IdRole).toString().isEmpty(), isHeader);
    }
}

TEST(NavModel, LooksUpIds)
{
    const NavModel model;
    const int row = model.rowOf(u"history"_s);
    ASSERT_GE(row, 0);
    EXPECT_EQ(model.idAt(row), u"history"_s);
    EXPECT_EQ(model.rowOf(u"nope"_s), -1);
    EXPECT_EQ(model.rowOf(QString()), -1);
    EXPECT_TRUE(model.idAt(-1).isEmpty());
    EXPECT_TRUE(model.idAt(10'000).isEmpty());
    EXPECT_FALSE(model.titleOf(u"history"_s).isEmpty());
}

TEST(NavModel, CountsSignalOnlyOnChange)
{
    NavModel model;
    const QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setCount(u"inbox"_s, 4);
    model.setCount(u"inbox"_s, 4);
    model.setCount(u"unknown"_s, 4);
    model.setCount(u"inbox"_s, -3); // clamps to 0
    EXPECT_EQ(changed.count(), 2);
    EXPECT_EQ(model.index(model.rowOf(u"inbox"_s)).data(NavModel::CountRole).toInt(), 0);
}
