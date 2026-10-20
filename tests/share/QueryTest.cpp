// SPDX-License-Identifier: GPL-3.0-or-later
// A peer's query, Nicotine+'s way (detail/Query, sharing note §4.2).
#include "share/detail/Query.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::share::detail::parseQuery;
using selecta::share::detail::ParsedQuery;

namespace {

TEST(ShareQuery, PlainExcludedAndPartialTerms)
{
    const ParsedQuery query = parseQuery(u"Daft Punk -live *mix");
    EXPECT_EQ(query.plain, QStringList({u"daft"_s, u"punk"_s}));
    EXPECT_EQ(query.excluded, QStringList({u"live"_s}));
    EXPECT_EQ(query.partial, QStringList({u"mix"_s}));
}

TEST(ShareQuery, EveryWordOfAMarkedTermKeepsTheMark)
{
    const ParsedQuery query = parseQuery(u"house -live-mix *step_by");
    EXPECT_EQ(query.plain, QStringList({u"house"_s}));
    EXPECT_EQ(query.excluded, QStringList({u"live"_s, u"mix"_s}));
    EXPECT_EQ(query.partial, QStringList({u"step"_s, u"by"_s}));
}

TEST(ShareQuery, MarksOnlyCountAtTheStartOfATerm)
{
    const ParsedQuery query = parseQuery(u"jay-z beat*it");
    EXPECT_EQ(query.plain, QStringList({u"jay"_s, u"z"_s, u"beat"_s, u"it"_s}));
    EXPECT_TRUE(query.excluded.isEmpty());
    EXPECT_TRUE(query.partial.isEmpty());
}

TEST(ShareQuery, WordsAreFoldedAndDistinct)
{
    const ParsedQuery query = parseQuery(u"RÖYKSOPP röyksopp Royksopp -Beyoncé -BEYONCE");
    EXPECT_EQ(query.plain, QStringList({u"royksopp"_s}));
    EXPECT_EQ(query.excluded, QStringList({u"beyonce"_s}));
}

TEST(ShareQuery, WhitespaceOfAnyKindSeparatesTerms)
{
    const ParsedQuery query = parseQuery(u"  one\ttwo\nthree four  ");
    EXPECT_EQ(query.plain, QStringList({u"one"_s, u"two"_s, u"three"_s, u"four"_s}));
}

TEST(ShareQuery, BareMarksAndPunctuationGiveNothing)
{
    const ParsedQuery query = parseQuery(u"- * -- ** ... !!");
    EXPECT_TRUE(query.plain.isEmpty());
    EXPECT_TRUE(query.excluded.isEmpty());
    EXPECT_TRUE(query.partial.isEmpty());
    EXPECT_FALSE(query.isAnswerable());
    EXPECT_FALSE(parseQuery(u"").isAnswerable());
}

TEST(ShareQuery, AnswerableNeedsAPlainWordOfThreeCodePoints)
{
    EXPECT_TRUE(parseQuery(u"abc").isAnswerable());
    EXPECT_TRUE(parseQuery(u"u2 one").isAnswerable());
    EXPECT_FALSE(parseQuery(u"u2").isAnswerable());
    EXPECT_FALSE(parseQuery(u"ac dc").isAnswerable());
    EXPECT_FALSE(parseQuery(u"AC/DC").isAnswerable());
    EXPECT_FALSE(parseQuery(u"*tronic").isAnswerable()); // a word ending isn't a whole word
    EXPECT_FALSE(parseQuery(u"-live").isAnswerable());   // exclusions alone answer nothing
    EXPECT_FALSE(parseQuery(u"-live *mix").isAnswerable());
    EXPECT_TRUE(parseQuery(u"日本語").isAnswerable());
    EXPECT_FALSE(parseQuery(u"日本").isAnswerable());
    EXPECT_FALSE(parseQuery(u"😀😀").isAnswerable()); // two code points, four code units
    EXPECT_TRUE(parseQuery(u"ßa").isAnswerable());    // folds to "ssa"
}

} // namespace
