// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/TextFold.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::meta::detail::foldKey;
using selecta::meta::detail::squash;

TEST(TextFold, CaseDiacriticsAndTransliteration)
{
    EXPECT_EQ(foldKey(u"Röyksopp"_s), u"royksopp"_s);
    EXPECT_EQ(foldKey(u"Beyoncé"_s), u"beyonce"_s);
    EXPECT_EQ(foldKey(u"Mø"_s), u"mo"_s);
    EXPECT_EQ(foldKey(u"Æther Œuvre"_s), u"aether oeuvre"_s);
    EXPECT_EQ(foldKey(u"Straße"_s), u"strasse"_s);
    EXPECT_EQ(foldKey(u"Łódź"_s), u"lodz"_s);
    EXPECT_EQ(foldKey(u"Þórunn Ðóra"_s), u"thorunn dora"_s);
    EXPECT_EQ(foldKey(u"DJ KOZE"_s), u"dj koze"_s);
}

TEST(TextFold, SymbolsAndPunctuation)
{
    EXPECT_EQ(foldKey(u"Simon & Garfunkel"_s), u"simon and garfunkel"_s);
    EXPECT_EQ(foldKey(u"Above+Beyond"_s), u"above and beyond"_s);
    EXPECT_EQ(foldKey(u"Ke$ha"_s), u"kesha"_s);
    EXPECT_EQ(foldKey(u"What's Going On"_s), u"whats going on"_s);
    EXPECT_EQ(foldKey(u"Don’t Stop"_s), u"dont stop"_s);
    EXPECT_EQ(foldKey(u"A.T.F.C."_s), u"atfc"_s);
    EXPECT_EQ(foldKey(u"Dr. Dre"_s), u"dr dre"_s);
    EXPECT_EQ(foldKey(u"Mr.Scruff"_s), u"mr scruff"_s);
    EXPECT_EQ(foldKey(u"Prodigy, The"_s), u"prodigy the"_s);
    EXPECT_EQ(foldKey(u"  Glue   (Extended Mix) "_s), u"glue extended mix"_s);
    EXPECT_EQ(foldKey(u"AC/DC"_s), u"ac dc"_s);
}

TEST(TextFold, CompatibilityFormsAndOtherScripts)
{
    EXPECT_EQ(foldKey(u"ＢＩＣＥＰ"_s), u"bicep"_s); // full width
    EXPECT_EQ(foldKey(u"ﬁve"_s), u"five"_s);         // ligature
    EXPECT_EQ(foldKey(u"Кино"_s), u"кино"_s);        // Cyrillic kept, case folded
    EXPECT_EQ(foldKey(u"坂本龍一"_s), u"坂本龍一"_s);
    EXPECT_EQ(foldKey(u"Glue 🎵 2017"_s), u"glue 2017"_s); // emoji are separators
}

TEST(TextFold, SquashAndBounds)
{
    EXPECT_EQ(squash(foldKey(u"Daft Punk"_s)), u"daftpunk"_s);
    EXPECT_EQ(squash(foldKey(u"AC/DC"_s)), u"acdc"_s);
    EXPECT_TRUE(foldKey(QString()).isEmpty());
    EXPECT_TRUE(foldKey(u" ... --- "_s).isEmpty());
    EXPECT_LE(foldKey(QString(5000, u'a')).size(), 1000);
}
