// SPDX-License-Identifier: GPL-3.0-or-later
// Typo tolerance (note §17.3–17.4): per-token OSA budgets and field alignment.
#include "match/Similarity.h"

#include "MatchTestSupport.h"
#include "match/Normalizer.h"
#include "match/detail/Alignment.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::FieldKind;
using selecta::match::FieldMatch;
using selecta::match::FieldMatcher;
using selecta::match::FieldMatchOptions;
using selecta::match::Normalizer;
using selecta::match::TokenComparison;
using selecta::match::TokenMatchKind;
using selecta::match::TokenRules;

namespace {

struct TokenCase
{
    const char *name = "";
    QString a;
    QString b;
    TokenMatchKind kind = TokenMatchKind::None;
    int edits = 0;
    bool needsCorroboration = false;
};

void PrintTo(const TokenCase &c, std::ostream *os)
{
    *os << c.name;
}

class TokenRulesTest : public testing::TestWithParam<TokenCase>
{};

TEST_P(TokenRulesTest, AppliesTheBudgets)
{
    const TokenCase &c = GetParam();
    for (const auto &[x, y] : {std::pair{c.a, c.b}, std::pair{c.b, c.a}}) {
        const TokenComparison comparison = TokenRules::compare(x, y);
        EXPECT_EQ(comparison.kind, c.kind) << x.toStdString() << " / " << y.toStdString();
        EXPECT_EQ(comparison.edits, c.edits);
        EXPECT_EQ(comparison.needsCorroboration, c.needsCorroboration);
    }
}

using K = TokenMatchKind;

INSTANTIATE_TEST_SUITE_P(
    Similarity, TokenRulesTest,
    testing::Values(TokenCase{"Equal", u"glue"_s, u"glue"_s, K::Exact, 0, false},
                    TokenCase{"ThreeLettersExactOnly_T60", u"max"_s, u"mix"_s, K::None, 0, false},
                    TokenCase{"FourLetterSubstitution_T59", u"noon"_s, u"neon"_s, K::None, 0, false},
                    TokenCase{"FourLetterSubstitution_T61", u"live"_s, u"love"_s, K::None, 0, false},
                    TokenCase{"FourLetterSubstitution_T87", u"kose"_s, u"koze"_s, K::None, 0, false},
                    TokenCase{"FourLetterDeletion_T55", u"bicp"_s, u"bicep"_s, K::Fuzzy, 1, true},
                    TokenCase{"FiveLetterDeletion_T79", u"behnd"_s, u"behind"_s, K::Fuzzy, 1, true},
                    TokenCase{"DoubledLetter_T56", u"gallaxy"_s, u"galaxy"_s, K::Skeleton, 1, false},
                    TokenCase{"Transposition_T57", u"firestatrer"_s, u"firestarter"_s, K::Fuzzy, 1, false},
                    TokenCase{"LongSubstitution_T58", u"firestorter"_s, u"firestarter"_s, K::Fuzzy, 1, false},
                    TokenCase{"ShortTokens_T62", u"mk"_s, u"mj"_s, K::None, 0, false},
                    TokenCase{"Numbers_T62", u"17"_s, u"18"_s, K::None, 0, false},
                    TokenCase{"OeDeletion_T68", u"roeyksopp"_s, u"royksopp"_s, K::Fuzzy, 1, false},
                    TokenCase{"UeDeletion_T69", u"mueller"_s, u"muller"_s, K::Fuzzy, 1, false},
                    TokenCase{"Leet_T65", u"deadmaus"_s, u"deadmau5"_s, K::Leet, 0, false},
                    TokenCase{"DigitsNeverEdited", u"blink182"_s, u"blink183"_s, K::None, 0, false},
                    TokenCase{"MidSubstitutionNeedsCorroboration", u"galaxy"_s, u"galoxy"_s, K::Fuzzy, 1, true},
                    TokenCase{"FirstLetterRule", u"galaxy"_s, u"xalaxy"_s, K::None, 0, false},
                    TokenCase{"LeadingTranspositionAllowed", u"ibcep"_s, u"bicep"_s, K::Fuzzy, 1, true},
                    TokenCase{"TwoEditsLong", u"symphonies"_s, u"symfonies"_s, K::Fuzzy, 2, false},
                    TokenCase{"ThreeEditsLong", u"symphonies"_s, u"symfonyes"_s, K::None, 0, false},
                    TokenCase{"TwoSubstitutionsLong", u"abcdefghij"_s, u"abxdefgyij"_s, K::None, 0, false},
                    TokenCase{"LongFirstLetterFree", u"firestarter"_s, u"virestarter"_s, K::Fuzzy, 1, false},
                    TokenCase{"LengthGap", u"bicep"_s, u"biceppp"_s, K::None, 0, false},
                    TokenCase{"Empty", QString(), u"glue"_s, K::None, 0, false}),
    [](const testing::TestParamInfo<TokenCase> &info) { return std::string(info.param.name); });

TEST(TokenRules, BudgetsFollowTheLength) // §17.3 table
{
    EXPECT_EQ(TokenRules::editBudget(u"dj"_s), 0);
    EXPECT_EQ(TokenRules::editBudget(u"mix"_s), 0);
    EXPECT_EQ(TokenRules::editBudget(u"glue"_s), 1);
    EXPECT_EQ(TokenRules::editBudget(u"bicep"_s), 1);
    EXPECT_EQ(TokenRules::editBudget(u"galaxy"_s), 1);
    EXPECT_EQ(TokenRules::editBudget(u"royksopp"_s), 1);
    EXPECT_EQ(TokenRules::editBudget(u"roeyksopp"_s), 2);
    EXPECT_EQ(TokenRules::editBudget(u"1999"_s), 0);
    EXPECT_EQ(TokenRules::editBudget(u"deadmau5"_s), 0);
    EXPECT_EQ(TokenRules::fieldEditBudget(12), 1);
    EXPECT_EQ(TokenRules::fieldEditBudget(13), 2);
}

TEST(TokenRules, LengthCountsCodePoints)
{
    EXPECT_EQ(TokenRules::length(u"abc"_s), 3);
    EXPECT_EQ(TokenRules::length(u"a\U0001F600b"_s), 3);
    EXPECT_EQ(TokenRules::skeleton(u"gallaxy"_s), u"galaxy"_s);
    EXPECT_EQ(TokenRules::leetFold(u"deadmau5"_s), u"deadmaus"_s);
    EXPECT_EQ(TokenRules::leetFold(u"1999"_s), u"1999"_s);
}

TEST(TokenRules, NoBudgetMakesThreeLetterWordsFuzzy) // property test (contract)
{
    selecta::match::test::Random random(1234);
    const QString letters = u"abcdefghijklmnopqrstuvwxyzéø"_s;
    for (int i = 0; i < 20000; ++i) {
        QString a;
        QString b;
        const qsizetype length = 1 + random.below(3);
        for (qsizetype k = 0; k < length; ++k) {
            a.append(letters.at(random.below(letters.size())));
        }
        const qsizetype lengthB = 1 + random.below(4);
        for (qsizetype k = 0; k < lengthB; ++k) {
            b.append(letters.at(random.below(letters.size())));
        }
        const TokenComparison comparison = TokenRules::compare(a, b);
        ASSERT_TRUE(comparison.kind == TokenMatchKind::None || comparison.kind == TokenMatchKind::Exact)
            << a.toStdString() << " / " << b.toStdString();
    }
}

FieldMatch compare(const QString &a, const QString &b, bool durationCorroborates = false)
{
    return FieldMatcher::compare(Normalizer::fieldKey(a, FieldKind::Title), Normalizer::fieldKey(b, FieldKind::Title),
                                 FieldMatchOptions{durationCorroborates});
}

TEST(FieldMatcher, OneTypoAmongExactWordsIsLikelyMaterial) // T79
{
    const FieldMatch match = compare(u"Leave The World Behnd"_s, u"Leave the World Behind"_s);
    EXPECT_TRUE(match.fuzzy);
    EXPECT_FALSE(match.exact);
    EXPECT_EQ(match.edits, 1);
    // Weights: leave 5, the 1 (a light word), world 5, behnd 5 | behind 6; "behnd" scores 0.85.
    EXPECT_NEAR(match.similarity, (15.25 + 16.1) / 33.0, 1e-9);
}

TEST(FieldMatcher, TooManyTyposMatchNothing) // T80
{
    const FieldMatch match = compare(u"Lev Teh Wrld Behnd"_s, u"Leave the World Behind"_s);
    EXPECT_DOUBLE_EQ(match.similarity, 0.0);
    // When the lengths agree, "wrld" and "behnd" align, but "lev" and "teh" are 3-letter words: far below the 0.85
    // title threshold of a likely match.
    EXPECT_NEAR(compare(u"Lev Teh Wrld Behnd"_s, u"Leave the World Behind"_s, true).similarity, 17.0 / 32.0, 1e-9);
}

TEST(FieldMatcher, ShortTokenEditsNeedCorroboration) // T55
{
    EXPECT_DOUBLE_EQ(compare(u"Bicp"_s, u"Bicep"_s).similarity, 0.0);
    const FieldMatch corroborated = compare(u"Bicp"_s, u"Bicep"_s, true);
    EXPECT_DOUBLE_EQ(corroborated.similarity, 0.85);
    EXPECT_TRUE(corroborated.fuzzy);
    // Another exact word of the field corroborates too.
    EXPECT_GT(compare(u"Bicp Glue"_s, u"Bicep Glue"_s).similarity, 0.9);
}

TEST(FieldMatcher, DoubledLettersNeedNoCorroboration) // T56
{
    EXPECT_DOUBLE_EQ(compare(u"Gallaxy"_s, u"Galaxy"_s).similarity, 0.85);
}

TEST(FieldMatcher, SquashedAndLeetKeysAreExact) // T53, T64, T65, T66, T78
{
    for (const auto &[a, b] :
         {std::pair{u"okumadrift"_s, u"Okuma Drift"_s}, std::pair{u"dead mau5"_s, u"deadmau5"_s},
          std::pair{u"AboveandBeyond"_s, u"Above & Beyond"_s}, std::pair{u"Tik Tok"_s, u"TiK ToK"_s},
          std::pair{u"P!nk"_s, u"Pink"_s}, std::pair{u"Deadmaus"_s, u"deadmau5"_s}}) {
        const FieldMatch match = compare(a, b);
        EXPECT_TRUE(match.exact) << a.toStdString() << " / " << b.toStdString();
        EXPECT_DOUBLE_EQ(match.similarity, 1.0);
    }
}

TEST(FieldMatcher, JoinedWordsAlign) // §17.4 step 3
{
    const FieldMatch match = compare(u"okumadrift lights"_s, u"Okuma Drift Light"_s);
    ASSERT_FALSE(match.pairs.isEmpty());
    EXPECT_EQ(match.pairs.first().comparison.kind, TokenMatchKind::Joined);
    EXPECT_GT(match.similarity, 0.9);
}

TEST(FieldMatcher, SquashedRouteForTyposInsideJoinedWords)
{
    const FieldMatch match = compare(u"okumadrft"_s, u"Okuma Drift"_s);
    EXPECT_EQ(match.route, FieldMatch::Route::SquashedFuzzy);
    EXPECT_DOUBLE_EQ(match.similarity, 0.85);
    // Digits must agree: "blink 182" is not "blink 183".
    EXPECT_LT(compare(u"Blink 182"_s, u"Blink 183"_s).similarity, 0.7);
}

TEST(FieldMatcher, ExtraWordsAreExplicit) // T75
{
    const FieldMatch match = compare(u"Love"_s, u"Love Story"_s);
    EXPECT_NEAR(match.similarity, 8.0 / 13.0, 1e-9);
    EXPECT_DOUBLE_EQ(match.coverageA, 1.0);
    EXPECT_NEAR(match.unexplainedB, 5.0 / 9.0, 1e-9);
}

TEST(FieldMatcher, LightWordsWeighLittle)
{
    EXPECT_GT(compare(u"The Galaxy"_s, u"Galaxy"_s).similarity, 0.9);
}

TEST(FieldMatcher, EmptyFieldsMatchNothing)
{
    EXPECT_DOUBLE_EQ(compare(QString(), u"x"_s).similarity, 0.0);
    EXPECT_DOUBLE_EQ(compare(QString(), QString()).similarity, 0.0);
}

TEST(FieldMatcher, SimilarityIsSymmetric) // property test (contract)
{
    const QStringList words{u"leave"_s,       u"the"_s,         u"world"_s,  u"behind"_s, u"behnd"_s,  u"okuma"_s,
                            u"drift"_s,       u"okumadrift"_s,  u"lights"_s, u"light"_s,  u"galaxy"_s, u"gallaxy"_s,
                            u"bicep"_s,       u"bicp"_s,        u"glue"_s,   u"love"_s,   u"live"_s,   u"story"_s,
                            u"firestarter"_s, u"firestorter"_s, u"182"_s,    u"183"_s,    u"mix"_s,    u"max"_s};
    selecta::match::test::Random random(99);
    for (int i = 0; i < 5000; ++i) {
        QStringList a;
        QStringList b;
        const qsizetype na = 1 + random.below(5);
        const qsizetype nb = 1 + random.below(5);
        for (qsizetype k = 0; k < na; ++k) {
            a.append(words.at(random.below(words.size())));
        }
        for (qsizetype k = 0; k < nb; ++k) {
            b.append(words.at(random.below(words.size())));
        }
        const bool corroborated = random.below(2) == 0;
        const FieldMatch forward = compare(a.join(u' '), b.join(u' '), corroborated);
        const FieldMatch backward = compare(b.join(u' '), a.join(u' '), corroborated);
        ASSERT_DOUBLE_EQ(forward.similarity, backward.similarity)
            << a.join(u' ').toStdString() << " / " << b.join(u' ').toStdString();
        ASSERT_EQ(forward.exact, backward.exact);
        ASSERT_GE(forward.similarity, 0.0);
        ASSERT_LE(forward.similarity, 1.0);
    }
}

// One random edit of §17.3's kinds (insertion, deletion, substitution, adjacent transposition, a doubled letter).
QString typo(const QString &word, selecta::match::test::Random &random)
{
    QString out = word;
    const qsizetype at = random.below(out.size());
    const QChar letter = QChar(u'a' + static_cast<char16_t>(random.below(26)));
    switch (random.below(5)) {
    case 0:
        out.insert(at, letter);
        break;
    case 1:
        out.remove(at, 1);
        break;
    case 2:
        out[at] = letter;
        break;
    case 3:
        if (at + 1 < out.size()) {
            std::swap(out[at], out[at + 1]);
        }
        break;
    default:
        out.insert(at, out.at(at));
        break;
    }
    return out;
}

// The matcher prunes candidates on these tests, so "can't reach" must never be wrong (property test).
TEST(FieldMatcher, PruningNeverRejectsAReachableScore)
{
    namespace detail = selecta::match::detail;
    const QStringList words{u"leave"_s,      u"the"_s,         u"world"_s,  u"behind"_s,   u"okuma"_s,    u"drift"_s,
                            u"okumadrift"_s, u"lights"_s,      u"galaxy"_s, u"bicep"_s,    u"glue"_s,     u"love"_s,
                            u"story"_s,      u"firestarter"_s, u"182"_s,    u"deadmau5"_s, u"deadmaus"_s, u"l0ve"_s,
                            u"mix"_s,        u"a"_s,           u"strobe"_s, u"ghosts"_s,   u"n"_s,        u"stuff"_s};
    selecta::match::test::Random random(4242);
    const auto field = [&](qsizetype count) {
        QStringList out;
        for (qsizetype k = 0; k < count; ++k) {
            const QString &word = words.at(random.below(words.size()));
            out.append(random.below(3) == 0 && word.size() > 1 ? typo(word, random) : word);
        }
        return out.join(random.below(4) == 0 ? QString() : u" "_s); // sometimes squashed together
    };
    for (int i = 0; i < 20000; ++i) {
        const selecta::match::FieldKey keyA = Normalizer::fieldKey(field(1 + random.below(5)), FieldKind::Title);
        const selecta::match::FieldKey keyB = Normalizer::fieldKey(field(1 + random.below(5)), FieldKind::Title);
        const detail::FieldView a = detail::fieldView(keyA);
        const detail::FieldView b = detail::fieldView(keyB);
        const FieldMatchOptions options{random.below(2) == 0};
        const double similarity = detail::compareFields(a, b, options, false).similarity;
        const std::span<const detail::AlignToken> spanA(a.tokens.data(), static_cast<std::size_t>(a.tokens.size()));
        const std::span<const detail::AlignToken> spanB(b.tokens.data(), static_cast<std::size_t>(b.tokens.size()));
        const double coverage = detail::align(spanA, spanB, options).coverageA();
        for (const double threshold : {similarity, coverage, 0.5, 0.85, 0.97}) {
            if (similarity >= threshold) {
                ASSERT_TRUE(detail::similarityMayReach(a, b, threshold))
                    << keyA.key.toStdString() << " / " << keyB.key.toStdString() << " at " << threshold;
            }
            if (coverage >= threshold) {
                ASSERT_TRUE(detail::coverageMayReach(spanA, spanB, threshold))
                    << keyA.key.toStdString() << " / " << keyB.key.toStdString() << " at " << threshold;
            }
        }
    }
}

TEST(FieldMatcher, BoundsLongFields)
{
    QString many;
    for (int i = 0; i < 500; ++i) {
        many += u"word%1 "_s.arg(i);
    }
    const FieldMatch match = compare(many, many + u"extra"_s);
    EXPECT_GE(match.similarity, 0.0);
}

} // namespace
