// SPDX-License-Identifier: GPL-3.0-or-later
// Token index (note §17.6) and artist dictionary (note §17.5 A).
#include "match/TokenIndex.h"

#include "MatchTestSupport.h"
#include "match/ArtistDictionary.h"

#include <gtest/gtest.h>
#include <set>

using namespace Qt::StringLiterals;
using selecta::match::ArtistDictionary;
using selecta::match::TokenIndex;
using selecta::match::TokenMatchKind;
using selecta::match::TokenRules;

namespace {

TokenIndex indexOf(const QStringList &terms)
{
    TokenIndex::Builder builder;
    for (qsizetype i = 0; i < terms.size(); ++i) {
        builder.add(terms.at(i), static_cast<quint32>(i));
    }
    return builder.build();
}

TEST(TokenIndex, PostingsAreSortedAndUnique)
{
    TokenIndex::Builder builder;
    builder.add(u"glue"_s, 7);
    builder.add(u"glue"_s, 3);
    builder.add(u"glue"_s, 7);
    builder.add(u"bicep"_s, 3);
    builder.add(QString(), 1); // ignored
    const TokenIndex index = builder.build();
    EXPECT_EQ(index.termCount(), 2);
    const quint32 glue = index.find(u"glue"_s);
    ASSERT_NE(glue, TokenIndex::kNoTerm);
    const auto postings = index.postings(glue);
    EXPECT_EQ(std::vector<quint32>(postings.begin(), postings.end()), (std::vector<quint32>{3, 7}));
    EXPECT_EQ(index.term(glue), u"glue"_s);
    EXPECT_EQ(index.find(u"missing"_s), TokenIndex::kNoTerm);
    EXPECT_TRUE(index.postings(TokenIndex::kNoTerm).empty());
    EXPECT_TRUE(index.postings(12345).empty());
    EXPECT_TRUE(index.term(12345).isEmpty());
    EXPECT_GT(index.memoryUsage(), 0U);
}

TEST(TokenIndex, BuilderIsEmptyAfterBuild)
{
    TokenIndex::Builder builder;
    builder.add(u"glue"_s, 1);
    const TokenIndex first = builder.build();
    const TokenIndex second = builder.build();
    EXPECT_EQ(first.termCount(), 1);
    EXPECT_EQ(second.termCount(), 0);
}

TEST(TokenIndex, EmptyIndexFindsNothing)
{
    const TokenIndex index;
    EXPECT_EQ(index.termCount(), 0);
    EXPECT_EQ(index.find(u"glue"_s), TokenIndex::kNoTerm);
    EXPECT_TRUE(index.fuzzyLookup(u"glue"_s).isEmpty());
}

TEST(TokenIndex, FindsTyposWithinTheRules) // T55, T57, T58, T68
{
    const TokenIndex index =
        indexOf({u"bicep"_s, u"firestarter"_s, u"royksopp"_s, u"galaxy"_s, u"neon"_s, u"deadmau5"_s, u"mix"_s});
    const auto first = [&index](const QString &token) -> QString {
        const auto hits = index.fuzzyLookup(token);
        return hits.isEmpty() ? QString() : index.term(hits.first().term).toString();
    };
    EXPECT_EQ(first(u"bicp"_s), u"bicep"_s);
    EXPECT_EQ(first(u"firestatrer"_s), u"firestarter"_s);
    EXPECT_EQ(first(u"firestorter"_s), u"firestarter"_s);
    EXPECT_EQ(first(u"roeyksopp"_s), u"royksopp"_s);
    EXPECT_EQ(first(u"gallaxy"_s), u"galaxy"_s);
    EXPECT_EQ(first(u"deadmaus"_s), u"deadmau5"_s);
    EXPECT_TRUE(first(u"noon"_s).isEmpty());  // substitution in a 4-letter word
    EXPECT_TRUE(first(u"max"_s).isEmpty());   // 3-letter words are exact only
    EXPECT_TRUE(first(u"bicep"_s).isEmpty()); // the exact term itself is not a fuzzy hit
}

// The delete index must return exactly what the token rules accept: compared with a brute-force scan.
TEST(TokenIndex, FuzzyLookupEqualsBruteForce)
{
    selecta::match::test::Random random(2026);
    const QString letters = u"abcdefghijklmnoprstuvy"_s;
    QStringList vocabulary;
    std::set<QString> seen;
    while (vocabulary.size() < 3000) {
        QString word;
        const qsizetype length = 3 + random.below(10);
        for (qsizetype k = 0; k < length; ++k) {
            word.append(letters.at(random.below(letters.size())));
        }
        if (seen.insert(word).second) {
            vocabulary.append(word);
        }
    }
    const TokenIndex index = indexOf(vocabulary);
    for (int i = 0; i < 600; ++i) {
        // A typo of a vocabulary word: one or two random edits.
        QString query = vocabulary.at(random.below(vocabulary.size()));
        const int edits = 1 + static_cast<int>(random.below(2));
        for (int e = 0; e < edits && query.size() > 1; ++e) {
            const qsizetype at = random.below(query.size());
            switch (random.below(4)) {
            case 0:
                query.remove(at, 1);
                break;
            case 1:
                query.insert(at, letters.at(random.below(letters.size())));
                break;
            case 2:
                query[at] = letters.at(random.below(letters.size()));
                break;
            default:
                if (at + 1 < query.size()) {
                    std::swap(query[at], query[at + 1]);
                }
                break;
            }
        }
        std::set<QString> expected;
        for (const QString &term : std::as_const(vocabulary)) {
            const auto comparison = TokenRules::compare(query, term);
            if (comparison.matched() && comparison.kind != TokenMatchKind::Exact) {
                expected.insert(term);
            }
        }
        std::set<QString> actual;
        const auto hits = index.fuzzyLookup(query, 10000);
        for (const auto &hit : hits) {
            actual.insert(index.term(hit.term).toString());
        }
        ASSERT_EQ(actual, expected) << "query " << query.toStdString();
    }
}

TEST(TokenIndex, HitsAreOrderedAndCapped)
{
    const TokenIndex index = indexOf({u"glues"_s, u"gluer"_s, u"glued"_s, u"gloomy"_s});
    const auto hits = index.fuzzyLookup(u"glue"_s, 2);
    ASSERT_EQ(hits.size(), 2);
    EXPECT_LE(hits.at(0).comparison.edits, hits.at(1).comparison.edits);
}

TEST(ArtistDictionary, FindsWholeNamesExactly)
{
    const ArtistDictionary dictionary({{u"Bicep"_s, 40}, {u"Daft Punk"_s, 10}, {u"Prodigy"_s, 5}});
    const auto bicep = dictionary.lookup(u"BICEP"_s);
    EXPECT_TRUE(bicep.exact());
    EXPECT_EQ(bicep.weight, 40);
    EXPECT_FALSE(bicep.memberOnly);
    EXPECT_TRUE(dictionary.lookup(u"The Prodigy"_s).exact()); // T45
    EXPECT_TRUE(dictionary.lookup(u"daftpunk"_s).exact());    // squashed
    EXPECT_FALSE(dictionary.lookup(u"Glue"_s).found());
    EXPECT_EQ(dictionary.size(), 3);
}

TEST(ArtistDictionary, MembersOfMultiArtistNamesAreWeaker)
{
    const ArtistDictionary dictionary({{u"Bicep & Hammer"_s, 3}, {u"A & B"_s, 2}});
    const auto hammer = dictionary.lookup(u"Hammer"_s);
    EXPECT_TRUE(hammer.exact());
    EXPECT_TRUE(hammer.memberOnly);
    EXPECT_FALSE(dictionary.lookup(u"A"_s).found()); // members shorter than 3 characters are no evidence
    EXPECT_TRUE(dictionary.lookup(u"A & B"_s).exact());
    EXPECT_TRUE(dictionary.lookup(u"Bicep and Hammer"_s).exact());
}

TEST(ArtistDictionary, FuzzyHitsNeedNoCorroboration)
{
    const ArtistDictionary dictionary({{u"Daft Punk"_s, 10}, {u"Bicep"_s, 4}});
    const auto daft = dictionary.lookup(u"Daft Pnk"_s);
    EXPECT_TRUE(daft.found());
    EXPECT_EQ(daft.edits, 1);
    // "bicp" against "bicep" would need corroboration (4–5 letters): no dictionary evidence.
    EXPECT_FALSE(dictionary.lookup(u"Bicp"_s).found());
}

TEST(ArtistDictionary, EmptyDictionaryFindsNothing)
{
    const ArtistDictionary dictionary;
    EXPECT_TRUE(dictionary.isEmpty());
    EXPECT_FALSE(dictionary.lookup(u"Bicep"_s).found());
    const ArtistDictionary blanks({{QString(), 3}, {u"!!!"_s, 1}});
    EXPECT_TRUE(blanks.isEmpty());
}

} // namespace
