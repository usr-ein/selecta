// SPDX-License-Identifier: GPL-3.0-or-later
// Network query words from a release and the "Search wider" ladder (note §17.7; etiquette note §7.3, §9.2).
#include "match/QueryPlanner.h"

#include "MatchTestSupport.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::QueryPlan;
using selecta::match::QueryPlanner;
using selecta::match::QueryStatus;
using selecta::match::QueryStep;
using selecta::match::ReleaseReference;

namespace {

ReleaseReference release(const QString &artist, const QString &title)
{
    return ReleaseReference{{artist}, title, {}, 0};
}

TEST(QueryPlanner, DiacriticsAreKeptUntilTheLadderFoldsThem) // T81
{
    const QueryPlanner planner;
    const ReleaseReference roeyksopp = release(u"Röyksopp"_s, u"Eple"_s);
    const QueryPlan plan = planner.plan(roeyksopp);
    EXPECT_EQ(plan.status, QueryStatus::Ready);
    EXPECT_EQ(plan.text, u"röyksopp eple"_s);
    const QList<QueryPlan> ladder = planner.widerLadder(roeyksopp);
    ASSERT_EQ(ladder.size(), 3);
    EXPECT_EQ(ladder.at(0).step, QueryStep::SpellingVariant);
    EXPECT_EQ(ladder.at(0).text, u"royksopp eple"_s);
    EXPECT_EQ(ladder.at(1).step, QueryStep::FewerWords);
    EXPECT_EQ(ladder.at(1).text, u"eple"_s);
    EXPECT_EQ(ladder.at(2).step, QueryStep::ArtistOnly);
    EXPECT_EQ(ladder.at(2).text, u"röyksopp"_s);
    for (const QueryPlan &step : ladder) {
        EXPECT_FALSE(step.text.contains(u'*')) << "never a wildcard";
    }
}

TEST(QueryPlanner, SymbolSplitsAreDropped) // T82
{
    const QueryPlan plan = QueryPlanner().plan(release(u"Ke$ha"_s, u"TiK ToK"_s));
    EXPECT_EQ(plan.text, u"tik tok"_s);
    EXPECT_FALSE(plan.notes.isEmpty());
}

TEST(QueryPlanner, ApostropheFragmentsAndTheFiveWordLimit) // T83
{
    const QueryPlan plan = QueryPlanner().plan(release(u"Swedish House Mafia"_s, u"Don't You Worry Child"_s));
    EXPECT_EQ(plan.text, u"swedish house mafia worry child"_s);
    EXPECT_LE(plan.words.size(), selecta::match::kMaxQueryWords);
    EXPECT_FALSE(plan.words.contains(u"dont"_s));
    EXPECT_FALSE(plan.words.contains(u"t"_s));
}

TEST(QueryPlanner, ExcludedPhrasesBlockTheSearch) // T84
{
    const QueryPlanner planner({u"GLUE"_s, u"  "_s});
    const QueryPlan plan = planner.plan(release(u"Bicep"_s, u"Glue"_s));
    EXPECT_EQ(plan.status, QueryStatus::Blocked);
    EXPECT_EQ(plan.blockedPhrase, u"glue"_s);
    EXPECT_FALSE(plan.isReady());
    EXPECT_TRUE(QueryPlanner({u"daft punk"_s}).plan(release(u"Bicep"_s, u"Glue"_s)).isReady());
}

TEST(QueryPlanner, TypedTyposAreRespelledFromTheRelease) // T86, SRCH-19
{
    const QueryPlanner planner;
    const QueryPlan typo = planner.plan(release(u"Bicep"_s, u"Glue"_s), QueryStep::Default, u"Bicpe Glue"_s);
    EXPECT_EQ(typo.text, u"bicep glue"_s);
    EXPECT_TRUE(typo.respelled);
    const QueryPlan same = planner.plan(release(u"Bicep"_s, u"Glue"_s), QueryStep::Default, u"glue BICEP"_s);
    EXPECT_FALSE(same.respelled);
}

TEST(QueryPlanner, RemixersJoinTheQuery)
{
    const QueryPlan plan = QueryPlanner().plan(release(u"Bicep"_s, u"Glue (Hammer Remix)"_s));
    EXPECT_EQ(plan.text, u"bicep glue hammer"_s);
    // Other version words never go to the network: the version is enforced by matching.
    EXPECT_EQ(QueryPlanner().plan(release(u"Bicep"_s, u"Glue (Extended Mix)"_s)).text, u"bicep glue"_s);
}

TEST(QueryPlanner, JoinedArtistIsASpellingVariant)
{
    const QList<QueryPlan> ladder = QueryPlanner().widerLadder(release(u"Okuma Drift"_s, u"Lights"_s));
    EXPECT_EQ(ladder.at(0).text, u"okumadrift lights"_s);
}

TEST(QueryPlanner, RepeatedQueriesAreUnchanged)
{
    const QList<QueryPlan> ladder = QueryPlanner().widerLadder(release(u"Bicep"_s, u"Glue"_s));
    EXPECT_EQ(ladder.at(0).status, QueryStatus::Unchanged); // nothing to fold or join
    EXPECT_EQ(ladder.at(1).text, u"glue"_s);
    EXPECT_EQ(ladder.at(2).text, u"bicep"_s);
}

TEST(QueryPlanner, NothingSelectiveLeft)
{
    const QueryPlan plan = QueryPlanner().plan(release(u"DJ"_s, u"X"_s));
    EXPECT_EQ(plan.status, QueryStatus::NoWords);
    EXPECT_TRUE(plan.text.isEmpty());
    EXPECT_EQ(QueryPlanner().plan(ReleaseReference{}).status, QueryStatus::NoWords);
}

TEST(QueryPlanner, TitlesMadeOfStopWordsKeepThem)
{
    EXPECT_EQ(QueryPlanner().plan(release(u"Bicep"_s, u"The Mix"_s)).text, u"bicep the mix"_s);
}

TEST(QueryPlanner, HyphenatedNamesKeepTheirWords)
{
    EXPECT_EQ(QueryPlanner().plan(release(u"Jay-Z"_s, u"Encore"_s)).text, u"jay encore"_s);
}

TEST(QueryPlanner, ResponderWordsFollowNicotine)
{
    EXPECT_EQ(QueryPlanner::responderWords(u"Ke$ha"_s), (QStringList{u"ke"_s, u"ha"_s}));
    EXPECT_EQ(QueryPlanner::responderWords(u"Don't"_s), (QStringList{u"don"_s, u"t"_s}));
    EXPECT_EQ(QueryPlanner::responderWords(u"AC/DC"_s), (QStringList{u"ac"_s, u"dc"_s}));
    EXPECT_EQ(QueryPlanner::responderWords(u"Beyoncé — Halo"_s), (QStringList{u"beyoncé"_s, u"halo"_s}));
    EXPECT_TRUE(QueryPlanner::responderWords(u"!!!"_s).isEmpty());
}

TEST(QueryPlanner, NeverEmitsWildcardsOrMoreThanFiveWords) // property test
{
    selecta::match::test::Random random(81);
    const QueryPlanner planner;
    for (int i = 0; i < 1000; ++i) {
        const ReleaseReference reference = release(selecta::match::test::randomText(random, 30),
                                                   selecta::match::test::randomText(random, 40) + u" *wild*"_s);
        for (const QueryStep step :
             {QueryStep::Default, QueryStep::SpellingVariant, QueryStep::FewerWords, QueryStep::ArtistOnly}) {
            const QueryPlan plan = planner.plan(reference, step);
            ASSERT_FALSE(plan.text.contains(u'*'));
            ASSERT_LE(plan.words.size(), selecta::match::kMaxQueryWords);
            ASSERT_LE(plan.text.size(), selecta::match::kMaxQueryLength);
            for (const QString &word : plan.words) {
                ASSERT_GE(word.size(), selecta::match::kMinQueryWordLength);
            }
        }
    }
}

} // namespace
