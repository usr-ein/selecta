// SPDX-License-Identifier: GPL-3.0-or-later
// ShareIndex::search(): Nicotine+'s matching (sharing note §4.2 table), excluded phrases, bounds and the cap.
#include "ShareTestSupport.h"
#include "share/ShareIndex.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::share::BuildResult;
using selecta::share::ShareIndex;
using selecta::share::test::displayPaths;
using selecta::share::test::Tree;
namespace collection = selecta::collection;
namespace slsk = selecta::slsk;

namespace {

const QString kOneMoreTime = u"Music\\Daft Punk\\Discovery (2001)\\01 - One More Time.mp3"_s;
const QString kAerodynamic = u"Music\\Daft Punk\\Discovery (2001)\\02 - Aerodynamic.mp3"_s;
const QString kRobotRock = u"Music\\Daft Punk\\Alive 2007\\01 - Robot Rock (Live).mp3"_s;
const QString kSoEasy = u"Music\\R\u00f6yksopp\\Melody A.M\\01 - So Easy.flac"_s;
const QString kFanRemix = u"Music\\Royksopp Fans\\so_easy_remix.mp3"_s;
const QString kElectronic = u"Music\\Various\\[2019] Techno.Compilation\\01. Artist – Electronic.mp3"_s;
const QString kHalo = u"Music\\Various\\Beyonc\u00e9 - Halo.mp3"_s;
const QString kU2 = u"Music\\Various\\u2 - one.mp3"_s;

class ShareSearch : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_tree = new Tree();
        for (const QString &path :
             {kOneMoreTime, kAerodynamic, kRobotRock, kSoEasy, kFanRemix, kElectronic, kHalo, kU2}) {
            QString relative = path.sliced(u"Music\\"_s.size());
            relative.replace(u'\\', u'/');
            s_tree->file(relative);
        }
        const QString track = s_tree->path(u"Daft Punk/Discovery (2001)/01 - One More Time.mp3"_s);
        const collection::ScannedFile facts =
            selecta::share::test::scanned(track, {selecta::match::Codec::Mp3, 320, false, 44100, 0}, 320'000);
        s_index =
            new std::shared_ptr<const ShareIndex>(selecta::share::test::buildTree(*s_tree, {{track, facts}}).index);
    }

    static void TearDownTestSuite()
    {
        delete s_index;
        delete s_tree;
        s_index = nullptr;
        s_tree = nullptr;
    }

    static QStringList search(const QString &query, const QStringList &phrases = {},
                              int cap = selecta::share::kDefaultSearchCap)
    {
        return displayPaths((*s_index)->search(query, phrases, cap));
    }

    static Tree *s_tree;
    static std::shared_ptr<const ShareIndex> *s_index;
};

Tree *ShareSearch::s_tree = nullptr;
std::shared_ptr<const ShareIndex> *ShareSearch::s_index = nullptr;

TEST_F(ShareSearch, EveryWordMustAppear)
{
    EXPECT_EQ(search(u"daft punk"_s), QStringList({kRobotRock, kOneMoreTime, kAerodynamic}));
    EXPECT_EQ(search(u"daft one"_s), QStringList({kOneMoreTime}));
    EXPECT_TRUE(search(u"daft easy"_s).isEmpty());
    EXPECT_TRUE(search(u"nothing-like-this"_s).isEmpty());
}

TEST_F(ShareSearch, WordsComeFromTheAliasFoldersNameAndExtension)
{
    EXPECT_EQ(search(u"music"_s).size(), 8);
    EXPECT_EQ(search(u"flac"_s), QStringList({kSoEasy}));
    EXPECT_EQ(search(u"discovery"_s), QStringList({kOneMoreTime, kAerodynamic}));
    EXPECT_EQ(search(u"2001 time"_s), QStringList({kOneMoreTime}));
}

TEST_F(ShareSearch, OnlyWholeWordsMatch)
{
    EXPECT_TRUE(search(u"aero"_s).isEmpty());
    EXPECT_TRUE(search(u"dynamic"_s).isEmpty());
    EXPECT_EQ(search(u"aerodynamic"_s), QStringList({kAerodynamic}));
}

TEST_F(ShareSearch, CaseDoesntMatter)
{
    EXPECT_EQ(search(u"DAFT PUNK"_s), search(u"daft punk"_s));
    EXPECT_EQ(search(u"Robot ROCK"_s), QStringList({kRobotRock}));
}

TEST_F(ShareSearch, ExclusionsRemoveFiles)
{
    EXPECT_EQ(search(u"daft punk -live"_s), QStringList({kOneMoreTime, kAerodynamic}));
    EXPECT_EQ(search(u"daft -discovery"_s), QStringList({kRobotRock}));
    EXPECT_EQ(search(u"daft punk -nowhere"_s).size(), 3); // an unknown word excludes nothing
    EXPECT_TRUE(search(u"daft -daft"_s).isEmpty());
    EXPECT_EQ(search(u"daft -alive-discovery"_s), QStringList()); // both words of the term are excluded
}

TEST_F(ShareSearch, AStarMatchesWordEndings)
{
    EXPECT_EQ(search(u"daft *dynamic"_s), QStringList({kAerodynamic}));
    EXPECT_EQ(search(u"music *tronic"_s), QStringList({kElectronic}));
    EXPECT_EQ(search(u"music *ROCK"_s), QStringList({kRobotRock}));
    EXPECT_EQ(search(u"daft *e"_s), QStringList({kRobotRock, kOneMoreTime})); // "alive", "live"; "one", "time"
    EXPECT_TRUE(search(u"music *zzz"_s).isEmpty());
    EXPECT_TRUE(search(u"*tronic"_s).isEmpty());                             // a word ending alone gets no answer
    EXPECT_EQ(search(u"music *aerodynamic"_s), QStringList({kAerodynamic})); // a whole word ends with itself
}

TEST_F(ShareSearch, DiacriticsAreFoldedOnBothSides)
{
    EXPECT_EQ(search(u"royksopp"_s), QStringList({kFanRemix, kSoEasy}));
    EXPECT_EQ(search(u"R\u00f6yksopp"_s), QStringList({kFanRemix, kSoEasy}));
    EXPECT_EQ(search(u"Ro\u0308yksopp easy"_s), QStringList({kFanRemix, kSoEasy})); // an NFD query
    EXPECT_EQ(search(u"beyonce"_s), QStringList({kHalo}));
    EXPECT_EQ(search(u"BEYONC\u00c9 halo"_s), QStringList({kHalo}));
}

TEST_F(ShareSearch, ShortWordsAloneGetNoAnswer)
{
    EXPECT_TRUE(search(u"u2"_s).isEmpty());
    EXPECT_TRUE(search(u"a m"_s).isEmpty());
    EXPECT_EQ(search(u"u2 one"_s), QStringList({kU2}));
    EXPECT_EQ(search(u"melody a m"_s), QStringList({kSoEasy}));
}

TEST_F(ShareSearch, OddSeparatorsInPathsSplitWords)
{
    EXPECT_EQ(search(u"techno compilation"_s), QStringList({kElectronic}));
    EXPECT_EQ(search(u"2019 artist electronic"_s), QStringList({kElectronic}));
    EXPECT_EQ(search(u"easy remix"_s), QStringList({kFanRemix}));
    EXPECT_EQ(search(u"daft-punk robot"_s), QStringList({kRobotRock})); // the query splits the same way
    EXPECT_EQ(search(u"(live) [robot]"_s), QStringList({kRobotRock}));
}

TEST_F(ShareSearch, RepeatedWordsCountOnce)
{
    EXPECT_EQ(search(u"daft daft DAFT"_s).size(), 3);
}

TEST_F(ShareSearch, AQueryWithAnExcludedPhraseGetsNoAnswer)
{
    EXPECT_TRUE(search(u"daft one more time"_s, {u"one more"_s}).isEmpty());
    EXPECT_TRUE(search(u"daft ONE MORE time"_s, {u"One More"_s}).isEmpty());
    EXPECT_EQ(search(u"daft one"_s, {u"one more"_s}), QStringList());
}

TEST_F(ShareSearch, FilesWhosePathHoldsAnExcludedPhraseAreLeftOut)
{
    EXPECT_EQ(search(u"daft punk"_s, {u"ROBOT rock"_s}), QStringList({kOneMoreTime, kAerodynamic}));
    EXPECT_EQ(search(u"daft punk"_s, {u"discovery (2001)"_s}), QStringList({kRobotRock}));
}

TEST_F(ShareSearch, EmptyExcludedPhrasesAreIgnored)
{
    EXPECT_EQ(search(u"daft punk"_s, {QString(), u""_s}).size(), 3);
}

TEST_F(ShareSearch, TheCapBoundsAnswers)
{
    EXPECT_EQ(search(u"music"_s, {}, 2), search(u"music"_s).mid(0, 2));
    EXPECT_EQ(search(u"music"_s, {}, 1).size(), 1);
    EXPECT_TRUE(search(u"music"_s, {}, 0).isEmpty());
    EXPECT_TRUE(search(u"music"_s, {}, -1).isEmpty());
    EXPECT_EQ(search(u"music"_s, {}, 1'000'000).size(),
              8); // clamped to the governor's ceiling, which 8 files don't reach
    // Files dropped for a phrase don't take a place under the cap.
    EXPECT_EQ(search(u"daft punk"_s, {u"robot"_s}, 1), QStringList({kOneMoreTime}));
}

TEST_F(ShareSearch, LongQueriesGetNoAnswer)
{
    const QString longest = u"daft"_s + QString(selecta::share::kMaxQueryChars - 4, u' ');
    EXPECT_EQ(search(longest).size(), 3);
    EXPECT_TRUE(search(longest + u' ').isEmpty());
}

TEST_F(ShareSearch, ResultsAreFullVirtualPathsInIndexOrderWithTheirFacts)
{
    const QList<slsk::FileEntry> results = (*s_index)->search(u"daft discovery"_s, {});
    ASSERT_EQ(results.size(), 2);
    EXPECT_EQ(results.at(0).name.display(), kOneMoreTime);
    EXPECT_EQ(results.at(0).extension, u"mp3"_s);
    EXPECT_EQ(results.at(0).size, static_cast<quint64>(QByteArrayLiteral("data").size()));
    EXPECT_EQ(results.at(0).attributes.bitrate, 320U);
    EXPECT_EQ(results.at(0).attributes.duration, 320U);
    EXPECT_EQ(results.at(1).name.display(), kAerodynamic);
    // Every answer is valid UTF-8 on the wire.
    EXPECT_EQ(results.at(0).name.raw, kOneMoreTime.toUtf8());
}

TEST(ShareSearchEmpty, AnEmptyIndexAnswersNothing)
{
    Tree tree;
    const BuildResult result = selecta::share::test::buildTree(tree);
    EXPECT_TRUE(result.index->search(u"music"_s, {}).isEmpty());
}

} // namespace
