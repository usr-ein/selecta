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

TEST(NavModel, SearchesSitUnderDiggingNewestFirst)
{
    NavModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    const int wishlist = model.rowOf(u"wishlist"_s);
    model.setSearch(1, u"okuma drift"_s, 0, true);
    model.setSearch(2, u"halden rue"_s, 0, true);
    EXPECT_EQ(model.rowOf(u"search:2"_s), wishlist);
    EXPECT_EQ(model.rowOf(u"search:1"_s), wishlist + 1);
    EXPECT_EQ(model.rowOf(u"wishlist"_s), wishlist + 2);
    const QModelIndex first = model.index(model.rowOf(u"search:1"_s));
    EXPECT_TRUE(first.data(NavModel::ClosableRole).toBool());
    EXPECT_TRUE(first.data(NavModel::BusyRole).toBool());

    model.setSearch(1, u"okuma drift"_s, 12, false);
    const QModelIndex updated = model.index(model.rowOf(u"search:1"_s));
    EXPECT_EQ(updated.data(NavModel::CountRole).toInt(), 12);
    EXPECT_FALSE(updated.data(NavModel::BusyRole).toBool());
    EXPECT_FALSE(model.index(model.rowOf(u"get"_s)).data(NavModel::ClosableRole).toBool());

    model.removeSearch(2);
    EXPECT_EQ(model.rowOf(u"search:2"_s), -1);
    EXPECT_EQ(model.rowOf(u"search:1"_s), wishlist);
    model.removeSearch(99); // unknown: nothing happens
    EXPECT_EQ(NavModel::searchOfItem(u"search:7"_s), 7);
    EXPECT_EQ(NavModel::searchOfItem(u"search:x"_s), 0);
    EXPECT_EQ(NavModel::searchOfItem(u"inbox"_s), 0);
}

TEST(NavModel, BrowsedPeersSitWithTheSearches)
{
    selecta::app::NavModel model;
    model.setSearch(1, u"Okuma Drift"_s, 12, false);
    model.setBrowse(u"vinyl_ghost"_s, 0, true);
    const QString id = selecta::app::NavModel::browseItemId(u"vinyl_ghost"_s);
    EXPECT_EQ(id, u"browse:vinyl_ghost"_s);
    EXPECT_EQ(selecta::app::NavModel::browseOfItem(id), u"vinyl_ghost"_s);
    EXPECT_TRUE(selecta::app::NavModel::browseOfItem(u"search:1"_s).isEmpty());
    const int row = model.rowOf(id);
    ASSERT_GE(row, 0);
    EXPECT_LT(row, model.rowOf(u"wishlist"_s)); // under Digging, before the Wishlist
    EXPECT_EQ(model.titleOf(id), u"vinyl_ghost"_s);
    EXPECT_TRUE(model.index(row).data(selecta::app::NavModel::ClosableRole).toBool());
    EXPECT_TRUE(model.index(row).data(selecta::app::NavModel::BusyRole).toBool());
    model.setBrowse(u"vinyl_ghost"_s, 340, false);
    EXPECT_EQ(model.index(model.rowOf(id)).data(selecta::app::NavModel::CountRole).toInt(), 340);
    EXPECT_FALSE(model.index(model.rowOf(id)).data(selecta::app::NavModel::BusyRole).toBool());
    model.removeBrowse(u"vinyl_ghost"_s);
    EXPECT_EQ(model.rowOf(id), -1);
    EXPECT_GE(model.rowOf(u"search:1"_s), 0);
}
