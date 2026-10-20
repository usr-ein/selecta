// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Icons.h"

#include <QImage>
#include <QList>
#include <QSize>

#include <array>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
namespace icons = selecta::app::icons;

namespace {
// The sizes packaging/logo/make-icons.sh draws: what X11 window managers and Windows ask for.
constexpr std::array kSizes{16, 24, 32, 48, 64, 128, 256};
} // namespace

TEST(AppIconTest, HasEverySize)
{
    const QIcon icon = icons::applicationIcon();
    ASSERT_FALSE(icon.isNull());
    const QList<QSize> sizes = icon.availableSizes();
    EXPECT_EQ(sizes.size(), qsizetype(kSizes.size()));
    for (const int size : kSizes) {
        EXPECT_TRUE(sizes.contains(QSize(size, size))) << size;
    }
}

TEST(AppIconTest, IsTheRoundRecordOnTransparency)
{
    for (const int size : kSizes) {
        const QImage image(u":/icons/selecta-%1.png"_s.arg(size));
        ASSERT_EQ(image.size(), QSize(size, size)) << size;
        EXPECT_EQ(qAlpha(image.pixel(0, 0)), 0) << size;                 // a corner, off the record
        EXPECT_EQ(qAlpha(image.pixel(size / 2, size / 4)), 255) << size; // the record, a quarter of the way down
        EXPECT_EQ(qAlpha(image.pixel(size / 2, size / 2)), 255) << size; // its centre
    }
}
