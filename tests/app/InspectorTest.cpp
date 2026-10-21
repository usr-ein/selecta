// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Inspector.h"

#include "app/views/SearchInspector.h"
#include "core/SearchResults.h"

#include <QFontMetrics>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>

#include <algorithm>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::Inspector;
using selecta::app::SearchInspector;

namespace {

// A result whose file name is one long word, as Soulseek names often are.
selecta::core::SearchResult longNamedResult()
{
    selecta::core::SearchHit hit;
    hit.peer = u"HarmoniciGelignite"_s;
    hit.path = QByteArrayLiteral(
        "@@music\\Acid\\202609\\Radioactive_Man-Jam_Out_The_Kicks-(AFT012)-24BIT-WEB-FLAC-2024-BABAS\\"
        "01-radioactive_man-under_the_counter-2f190214.flac");
    hit.extension = u"flac"_s;
    hit.size = 65'700'000;
    hit.attributes.duration = 368;
    selecta::core::SearchResult result = selecta::core::filterableResult(hit);
    result.index = 0;
    return result;
}

} // namespace

// Beta notes, 2026-10-20: "Not in your collection, Inbox or download history" was cut at the inspector's edge. A long
// file name widened the details past the panel, so every line ran under the edge.
TEST(InspectorTest, TheSearchInspectorWrapsItsTextToThePanel)
{
    Inspector inspector;
    auto *panel = new SearchInspector;
    inspector.addPanel(u"search"_s, panel);
    inspector.showPanelFor(u"search"_s);
    // As narrow as the panel's widest button lets it be (its text can't wrap).
    const auto *folderButton = panel->findChild<QWidget *>(u"inspector.search.downloadFolder"_s);
    ASSERT_NE(folderButton, nullptr);
    inspector.resize(std::max(inspector.minimumWidth(), folderButton->sizeHint().width() + 40), 700);
    inspector.setAttribute(Qt::WA_DontShowOnScreen);
    inspector.show();
    panel->showResult(longNamedResult(), 0);
    QCoreApplication::processEvents();

    auto *scroll = panel->findChild<QScrollArea *>();
    ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    ASSERT_TRUE(QTest::qWaitFor([&] { return scroll->widget()->width() > 0; }, 1000));
    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width()); // nothing runs under the edge

    auto *ownership = panel->findChild<QLabel *>(u"inspector.search.ownership"_s);
    ASSERT_NE(ownership, nullptr);
    ASSERT_EQ(ownership->text(), u"Not in your collection, Inbox or download history."_s);
    EXPECT_LE(ownership->width(), scroll->viewport()->width());
    const QFontMetrics metrics(ownership->font());
    ASSERT_GT(metrics.horizontalAdvance(ownership->text()), ownership->width()); // it can't fit on one line…
    EXPECT_GE(ownership->height(), 2 * metrics.lineSpacing());                   // …so it takes two or more

    // The long name is cut at the edge, not wrapped: the whole of it is in the tooltip.
    auto *file = panel->findChild<QLabel *>(u"inspector.search.file"_s);
    ASSERT_NE(file, nullptr);
    EXPECT_LE(file->width(), scroll->viewport()->width());
    EXPECT_TRUE(file->toolTip().contains(u"01-radioactive_man-under_the_counter-2f190214.flac"_s));
}

TEST(InspectorTest, FitToWidthToleratesAnEmptyScrollArea)
{
    Inspector::fitToWidth(nullptr);
    QScrollArea empty;
    Inspector::fitToWidth(&empty);
    EXPECT_EQ(empty.horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}
