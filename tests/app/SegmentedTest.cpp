// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Segmented.h"

#include <QAbstractButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::SegmentedControl;

TEST(SegmentedControl, OneSegmentIsChosenAtATime)
{
    SegmentedControl control;
    EXPECT_EQ(control.currentIndex(), -1);
    EXPECT_EQ(control.addSegment(u"Transfers"_s, u"seg.0"_s), 0);
    EXPECT_EQ(control.addSegment(u"Peers"_s, u"seg.1"_s), 1);
    EXPECT_EQ(control.addSegment(u"Log"_s, u"seg.2"_s), 2);
    EXPECT_EQ(control.count(), 3);
    EXPECT_EQ(control.currentIndex(), 0); // the first one added
    control.setAttribute(Qt::WA_DontShowOnScreen);
    control.show();

    QSignalSpy changed(&control, &SegmentedControl::currentChanged);
    auto *peers = control.findChild<QAbstractButton *>(u"seg.1"_s);
    ASSERT_NE(peers, nullptr);
    EXPECT_EQ(peers->accessibleName(), u"Peers"_s);
    EXPECT_TRUE(peers->focusPolicy() & Qt::TabFocus); // the keyboard reaches each segment
    QTest::mouseClick(peers, Qt::LeftButton);
    EXPECT_EQ(control.currentIndex(), 1);
    ASSERT_EQ(changed.count(), 1);
    EXPECT_EQ(changed.at(0).at(0).toInt(), 1);

    control.setCurrentIndex(2); // from the program: no signal
    EXPECT_EQ(control.currentIndex(), 2);
    EXPECT_EQ(changed.count(), 1);
    control.setCurrentIndex(7); // out of range: nothing changes
    EXPECT_EQ(control.currentIndex(), 2);
    EXPECT_GT(control.sizeHint().width(), 3 * 30);
}

TEST(SegmentedControl, StandsInForATabWidgetsTabs)
{
    QWidget host;
    auto *tabs = new QTabWidget(&host);
    tabs->addTab(new QWidget, u"Conversations"_s);
    tabs->addTab(new QWidget, u"People"_s);
    SegmentedControl *control = SegmentedControl::forTabs(tabs, u"messages.switcher"_s, &host);
    host.setAttribute(Qt::WA_DontShowOnScreen);
    host.show();
    EXPECT_EQ(control->objectName(), u"messages.switcher"_s);
    EXPECT_EQ(control->count(), 2);
    EXPECT_FALSE(tabs->tabBar()->isVisible()); // the control replaces the tabs
    auto *people = control->findChild<QAbstractButton *>(u"messages.switcher.1"_s);
    ASSERT_NE(people, nullptr);
    EXPECT_EQ(people->text(), u"People"_s);
    QTest::mouseClick(people, Qt::LeftButton);
    EXPECT_EQ(tabs->currentIndex(), 1); // the control turns the page
    tabs->setCurrentIndex(0);
    EXPECT_EQ(control->currentIndex(), 0); // and follows the page turned elsewhere
}
