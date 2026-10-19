// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/QueryHygiene.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::governor::cleanQuery;
using selecta::governor::queryKey;
using selecta::governor::Refusal;

TEST(QueryHygiene, CollapsesSpacesAndDropsOneLetterWords)
{
    const auto clean = cleanQuery(u"  Okuma   Drift  a  -b  Neon Rain "_s, {});
    EXPECT_EQ(clean.refusal, Refusal::None);
    EXPECT_EQ(clean.query, u"Okuma Drift Neon Rain"_s);
}

TEST(QueryHygiene, KeepsLongerExclusions)
{
    EXPECT_EQ(cleanQuery(u"neon rain -live"_s, {}).query, u"neon rain -live"_s);
}

TEST(QueryHygiene, RefusesTooShort)
{
    EXPECT_EQ(cleanQuery(u""_s, {}).refusal, Refusal::QueryTooShort);
    EXPECT_EQ(cleanQuery(u"a b c"_s, {}).refusal, Refusal::QueryTooShort); // every word dropped
    EXPECT_EQ(cleanQuery(u"ab"_s, {}).refusal, Refusal::QueryTooShort);
    EXPECT_EQ(cleanQuery(u"abc"_s, {}).refusal, Refusal::None);
}

TEST(QueryHygiene, RefusesTooLong)
{
    const QString longWord(257, u'x');
    EXPECT_EQ(cleanQuery(longWord, {}).refusal, Refusal::QueryTooLong);
    EXPECT_EQ(cleanQuery(QString(256, u'x'), {}).refusal, Refusal::None);
}

TEST(QueryHygiene, RefusesExcludedPhrasesWithoutEvadingThem)
{
    const QStringList excluded{u"Bad Phrase"_s, u"   "_s};
    const auto clean = cleanQuery(u"artist BAD   phrase remix"_s, excluded);
    EXPECT_EQ(clean.refusal, Refusal::QueryExcludedPhrase);
    EXPECT_EQ(clean.excludedPhrase, u"Bad Phrase"_s);
    EXPECT_TRUE(clean.query.isEmpty());
    EXPECT_EQ(cleanQuery(u"artist good remix"_s, excluded).refusal, Refusal::None); // a blank phrase matches nothing
}

TEST(QueryHygiene, KeyIgnoresOrderCaseAndRepeats)
{
    EXPECT_EQ(queryKey(u"Neon Rain okuma"_s), queryKey(u"okuma neon rain RAIN"_s));
    EXPECT_NE(queryKey(u"neon rain"_s), queryKey(u"neon rain dub"_s));
}
