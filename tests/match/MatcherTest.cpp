// SPDX-License-Identifier: GPL-3.0-or-later
// Ownership matching (note §9, §10, §17.5): the edge-case table's ownership rows against synthetic libraries.
#include "match/Matcher.h"

#include "MatchTestSupport.h"
#include "match/ArtistDictionary.h"
#include "match/MatchIndex.h"
#include "match/RemotePathParser.h"

#include <gtest/gtest.h>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using selecta::match::AudioQuality;
using selecta::match::Codec;
using selecta::match::HistoryEntry;
using selecta::match::LibraryTrack;
using selecta::match::LocalTrackQuery;
using selecta::match::Matcher;
using selecta::match::MatchIndex;
using selecta::match::MatchIndexInput;
using selecta::match::MatchOptions;
using selecta::match::MatchResult;
using selecta::match::ParseContext;
using selecta::match::ParsedRemote;
using selecta::match::Relation;
using selecta::match::RemoteAttributes;
using selecta::match::RemoteFile;
using selecta::match::RemotePathParser;
using selecta::match::Source;
using selecta::match::Tier;

namespace {

struct Owned
{
    Owned(quint64 id, QString artist, QString title, qint64 ms, QString album = {}, Source source = Source::Collection,
          const AudioQuality &quality = {Codec::Mp3, 320, false, 44100, 0})
        : id(id)
        , artist(std::move(artist))
        , title(std::move(title))
        , ms(ms)
        , album(std::move(album))
        , source(source)
        , quality(quality)
    {}

    quint64 id;
    QString artist;
    QString title;
    qint64 ms;
    QString album;
    Source source;
    AudioQuality quality;
};

LibraryTrack track(const Owned &owned)
{
    LibraryTrack track;
    track.id = owned.id;
    track.source = owned.source;
    track.artist = owned.artist;
    track.title = owned.title;
    track.album = owned.album;
    track.durationMs = owned.ms;
    track.quality = owned.quality;
    return track;
}

std::shared_ptr<const MatchIndex> indexOf(const QList<Owned> &library, const QList<HistoryEntry> &history = {})
{
    MatchIndexInput input;
    for (const Owned &owned : library) {
        input.tracks.append(track(owned));
    }
    input.history = history;
    input.generation = 7;
    return MatchIndex::build(input);
}

struct OwnershipCase
{
    OwnershipCase(const char *name, QString path, int length, QList<Owned> library, Tier tier, quint64 matched = 0,
                  bool otherVersion = false, QStringList siblings = {})
        : name(name)
        , path(std::move(path))
        , length(length)
        , library(std::move(library))
        , tier(tier)
        , matched(matched)
        , otherVersion(otherVersion)
        , siblings(std::move(siblings))
    {}

    const char *name;
    QString path;
    int length;
    QList<Owned> library;
    Tier tier;
    quint64 matched;   // the collection record expected at tier ≥ likely
    bool otherVersion; // result.otherVersion expected
    QStringList siblings;
};

void PrintTo(const OwnershipCase &c, std::ostream *os)
{
    *os << c.name;
}

MatchResult matchPath(const std::shared_ptr<const MatchIndex> &index, const QString &path, int length,
                      const QStringList &siblings = {}, quint64 size = 0, const RemoteAttributes &attributes = {},
                      const QString &peer = {})
{
    const ParsedRemote parsed = RemotePathParser::parse(path, siblings, ParseContext{&index->artists(), nullptr});
    RemoteFile file;
    file.peer = peer;
    file.path = path;
    file.size = size;
    file.attributes = attributes;
    file.attributes.durationSec = length;
    return Matcher(index).match(parsed, file, MatchOptions{true});
}

class OwnershipTest : public testing::TestWithParam<OwnershipCase>
{};

TEST_P(OwnershipTest, DecidesTheTier)
{
    const OwnershipCase &c = GetParam();
    const auto index = indexOf(c.library);
    const MatchResult result = matchPath(index, c.path, c.length, c.siblings);
    const QString why = result.explanation.join(u'\n');
    EXPECT_EQ(result.collection.tier, c.tier) << why.toStdString();
    if (c.tier != Tier::None) {
        EXPECT_EQ(result.collection.id, c.matched) << why.toStdString();
        EXPECT_TRUE(result.collection.relation == Relation::Same || result.collection.relation == Relation::Upgrade);
    } else {
        EXPECT_FALSE(result.collection.isSet()) << why.toStdString();
    }
    EXPECT_EQ(result.otherVersion.isSet(), c.otherVersion) << why.toStdString();
    EXPECT_EQ(result.generation, 7U);
}

using O = Owned;
constexpr Tier kExact = Tier::Exact;
constexpr Tier kLikely = Tier::Likely;
constexpr Tier kNone = Tier::None;

INSTANTIATE_TEST_SUITE_P(
    EdgeCases, OwnershipTest,
    testing::Values(
        OwnershipCase{"T01_FolderLayout",
                      u"Music\\Daft Punk\\Discovery (2001)\\03 - Digital Love.flac"_s,
                      301,
                      {O{1, u"Daft Punk"_s, u"Digital Love"_s, 301000}},
                      kExact,
                      1},
        OwnershipCase{"T02_UnspecifiedVsOriginal",
                      u"DJ\\Bicep - Glue (Original Mix).mp3"_s,
                      270,
                      {O{1, u"Bicep"_s, u"Glue"_s, 269000}},
                      kExact,
                      1},
        OwnershipCase{"T03_ExtendedIsAnotherVersion",
                      u"DJ\\Bicep - Glue (Extended Mix).mp3"_s,
                      392,
                      {O{1, u"Bicep"_s, u"Glue"_s, 269000}},
                      kNone,
                      0,
                      true},
        OwnershipCase{"T04_ClassConflict",
                      u"Xenia - Track (Radio Edit).mp3"_s,
                      205,
                      {O{1, u"Xenia"_s, u"Track (Extended Mix)"_s, 410000}},
                      kNone,
                      0,
                      true},
        OwnershipCase{"T05_RemixerConflict",
                      u"Xenia - Track (Yello Remix).mp3"_s,
                      380,
                      {O{1, u"Xenia"_s, u"Track (Zara Remix)"_s, 378000}},
                      kNone,
                      0,
                      true},
        OwnershipCase{"T06_SiblingOrder",
                      u"Folder\\01 - Alpha - Artist.mp3"_s,
                      200,
                      {O{1, u"Artist"_s, u"Alpha"_s, 200000}},
                      kExact,
                      1,
                      false,
                      {u"02 - Beta - Artist.mp3"_s, u"03 - Gamma - Artist.mp3"_s, u"04 - Delta - Artist.mp3"_s}},
        OwnershipCase{
            "T07_HyphenatedArtist", u"Jay-Z - Song.mp3"_s, 240, {O{1, u"Jay-Z"_s, u"Song"_s, 240000}}, kExact, 1},
        OwnershipCase{
            "T09_DiacriticFolding", u"Royksopp - Eple.mp3"_s, 222, {O{1, u"Röyksopp"_s, u"Eple"_s, 222000}}, kExact, 1},
        OwnershipCase{"T12_Featured",
                      u"Artist feat. Guest - Title.mp3"_s,
                      200,
                      {O{1, u"Artist"_s, u"Title (feat. Guest)"_s, 200000}},
                      kExact,
                      1},
        OwnershipCase{"T13_AmpersandAnd",
                      u"Above & Beyond - Sun & Moon.mp3"_s,
                      334,
                      {O{1, u"Above and Beyond"_s, u"Sun and Moon"_s, 334000}},
                      kExact,
                      1},
        OwnershipCase{"T14_ArtistSets", u"B & A - Title.mp3"_s, 300, {O{1, u"A & B"_s, u"Title"_s, 300000}}, kExact, 1},
        OwnershipCase{"T15_CoverVersion",
                      u"Cover Band - Hallelujah.mp3"_s,
                      410,
                      {O{1, u"Jeff Buckley"_s, u"Hallelujah"_s, 413000}},
                      kNone},
        OwnershipCase{"T16_NoLength", u"Artist - Title.mp3"_s, 0, {O{1, u"Artist"_s, u"Title"_s, 300000}}, kLikely, 1},
        OwnershipCase{
            "T17_DeltaThree", u"Artist - Title.mp3"_s, 363, {O{1, u"Artist"_s, u"Title"_s, 360400}}, kExact, 1},
        OwnershipCase{
            "T17_DeltaFour", u"Artist - Title.mp3"_s, 364, {O{1, u"Artist"_s, u"Title"_s, 360400}}, kLikely, 1},
        OwnershipCase{"T18_BetweenLikelyAndFar",
                      u"Artist - Title.mp3"_s,
                      385,
                      {O{1, u"Artist"_s, u"Title"_s, 360000}},
                      kNone,
                      0,
                      true},
        OwnershipCase{
            "T19_UnparseableName", u"Rips\\Track 01.wav"_s, 300, {O{1, u"Artist"_s, u"Track"_s, 300000}}, kNone},
        OwnershipCase{"T26_WebJunk",
                      u"www.site.ru - Artist - Title (Official Video) [HQ].mp3"_s,
                      300,
                      {O{1, u"Artist"_s, u"Title"_s, 300000}},
                      kExact,
                      1},
        OwnershipCase{"T28_SquashedAcDc",
                      u"AC_DC - Back In Black.mp3"_s,
                      255,
                      {O{1, u"AC/DC"_s, u"Back in Black"_s, 255000}},
                      kExact,
                      1},
        OwnershipCase{"T29_Apostrophe",
                      u"Marvin Gaye - Whats Going On.mp3"_s,
                      233,
                      {O{1, u"Marvin Gaye"_s, u"What's Going On"_s, 233000}},
                      kExact,
                      1},
        OwnershipCase{"T30_Remaster",
                      u"Artist - Title (2011 Remaster).flac"_s,
                      300,
                      {O{1, u"Artist"_s, u"Title"_s, 299000}},
                      kExact,
                      1},
        OwnershipCase{"T31_UnknownBracketText",
                      u"Artist - Title (Intro).mp3"_s,
                      60,
                      {O{1, u"Artist"_s, u"Title"_s, 60000}},
                      kLikely,
                      1},
        OwnershipCase{"T34_LengthPicksTheVersion",
                      u"Artist - Title.mp3"_s,
                      391,
                      {O{1, u"Artist"_s, u"Title (Original Mix)"_s, 300000},
                       O{2, u"Artist"_s, u"Title (Extended Mix)"_s, 391000}},
                      kLikely,
                      2,
                      true},
        OwnershipCase{"T45_ArticleSuffix",
                      u"Prodigy, The - Firestarter.mp3"_s,
                      283,
                      {O{1, u"The Prodigy"_s, u"Firestarter"_s, 283000}},
                      kExact,
                      1},
        OwnershipCase{"T46_SelfRemixIsAnotherVersion",
                      u"Artist - Title (Artist Remix).mp3"_s,
                      300,
                      {O{1, u"Artist"_s, u"Title (Original Mix)"_s, 300000}},
                      kNone,
                      0,
                      true},
        OwnershipCase{"T47_MojibakeRepaired",
                      u"RÃ¶yksopp - Eple.mp3"_s,
                      222,
                      {O{1, u"Röyksopp"_s, u"Eple"_s, 222000}},
                      kExact,
                      1}),
    [](const testing::TestParamInfo<OwnershipCase> &info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    Typos, OwnershipTest,
    testing::Values(
        OwnershipCase{"T53_JoinedArtist",
                      u"Music\\okumadrift - lights.mp3"_s,
                      240,
                      {O{1, u"Okuma Drift"_s, u"Lights"_s, 240000}},
                      kExact,
                      1},
        OwnershipCase{"T54_CamelCase",
                      u"OkumaDrift - Lights.mp3"_s,
                      240,
                      {O{1, u"Okuma Drift"_s, u"Lights"_s, 240000}},
                      kExact,
                      1},
        OwnershipCase{
            "T55_MissingLetter", u"Bicp - Glue.mp3"_s, 269, {O{1, u"Bicep"_s, u"Glue"_s, 269000}}, kLikely, 1},
        OwnershipCase{
            "T56_DoubledLetter", u"Artist - Gallaxy.mp3"_s, 0, {O{1, u"Artist"_s, u"Galaxy"_s, 250000}}, kLikely, 1},
        OwnershipCase{"T57_Transposition",
                      u"The Prodigy - Firestatrer.mp3"_s,
                      283,
                      {O{1, u"The Prodigy"_s, u"Firestarter"_s, 283000}},
                      kLikely,
                      1},
        OwnershipCase{"T58_Substitution",
                      u"Prodigy - Firestorter.mp3"_s,
                      283,
                      {O{1, u"The Prodigy"_s, u"Firestarter"_s, 283000}},
                      kLikely,
                      1},
        OwnershipCase{"T59_NoonIsNotNeon", u"Artist - Noon.mp3"_s, 240, {O{1, u"Artist"_s, u"Neon"_s, 240000}}, kNone},
        OwnershipCase{"T60_ShortWordsExact", u"Artist - Max.mp3"_s, 200, {O{1, u"Artist"_s, u"Mix"_s, 200000}}, kNone},
        OwnershipCase{"T61_LiveIsNotLove", u"Artist - Live.mp3"_s, 200, {O{1, u"Artist"_s, u"Love"_s, 200000}}, kNone},
        OwnershipCase{"T62_ShortArtistExact", u"MK - 17.mp3"_s, 200, {O{1, u"MK"_s, u"17"_s, 200000}}, kExact, 1},
        OwnershipCase{"T62_ShortArtistDiffers", u"MJ - 17.mp3"_s, 200, {O{1, u"MK"_s, u"17"_s, 200000}}, kNone},
        OwnershipCase{"T63_PartNumbering",
                      u"Artist - Title Part II.mp3"_s,
                      300,
                      {O{1, u"Artist"_s, u"Title Pt. 2"_s, 300000}},
                      kExact,
                      1},
        OwnershipCase{
            "T64_DollarSign", u"Ke$ha - Tik Tok.mp3"_s, 200, {O{1, u"Kesha"_s, u"TiK ToK"_s, 200000}}, kExact, 1},
        OwnershipCase{
            "T65_LeetDigits", u"Deadmaus - Strobe.mp3"_s, 600, {O{1, u"deadmau5"_s, u"Strobe"_s, 600000}}, kExact, 1},
        OwnershipCase{
            "T66_SplitName", u"dead mau5 - Strobe.mp3"_s, 600, {O{1, u"deadmau5"_s, u"Strobe"_s, 600000}}, kExact, 1},
        OwnershipCase{"T67_Folding",
                      u"Sigur Ros - Hoppipolla.mp3"_s,
                      268,
                      {O{1, u"Sigur Rós"_s, u"Hoppípolla"_s, 268000}},
                      kExact,
                      1},
        OwnershipCase{
            "T68_OeSpelling", u"Roeyksopp - Eple.mp3"_s, 222, {O{1, u"Röyksopp"_s, u"Eple"_s, 222000}}, kLikely, 1},
        OwnershipCase{
            "T69_UeSpelling", u"Mueller - Title.mp3"_s, 200, {O{1, u"Müller"_s, u"Title"_s, 200000}}, kLikely, 1},
        OwnershipCase{"T70_ReversedName", u"Glue - Bicep.mp3"_s, 269, {O{1, u"Bicep"_s, u"Glue"_s, 269000}}, kExact, 1},
        OwnershipCase{"T72_ReversedFolder",
                      u"Isles - Bicep\\03 - Apricots.flac"_s,
                      330,
                      {O{1, u"Bicep"_s, u"Apricots"_s, 330000, u"Isles"_s}},
                      kExact,
                      1},
        OwnershipCase{"T77_AcronymDots", u"M.K. - Title.mp3"_s, 200, {O{1, u"MK"_s, u"Title"_s, 200000}}, kExact, 1},
        OwnershipCase{"T78_JoinedWithAnd",
                      u"AboveandBeyond - Sun and Moon.mp3"_s,
                      334,
                      {O{1, u"Above & Beyond"_s, u"Sun & Moon"_s, 334000}},
                      kExact,
                      1},
        OwnershipCase{"T79_OneTypo",
                      u"Artist - Leave The World Behnd.mp3"_s,
                      300,
                      {O{1, u"Artist"_s, u"Leave the World Behind"_s, 300000}},
                      kLikely,
                      1},
        OwnershipCase{"T80_TooManyTypos",
                      u"Artist - Lev Teh Wrld Behnd.mp3"_s,
                      300,
                      {O{1, u"Artist"_s, u"Leave the World Behind"_s, 300000}},
                      kNone},
        OwnershipCase{"T87_ShortWordSubstitution",
                      u"DJ Kose - Pick Up.mp3"_s,
                      240,
                      {O{1, u"DJ Koze"_s, u"Pick Up"_s, 240000}},
                      kNone}),
    [](const testing::TestParamInfo<OwnershipCase> &info) { return std::string(info.param.name); });

TEST(Matcher, NfdPathsMatchNfcTags) // T35: a macOS peer sends NFD
{
    const QString nfc = u"Beyoncé"_s.normalized(QString::NormalizationForm_C);
    const auto index = indexOf({O{1, nfc, u"Halo"_s, 261000}});
    const QString path = QString(nfc + u" - Halo.mp3"_s).normalized(QString::NormalizationForm_D);
    ASSERT_NE(path, QString(nfc + u" - Halo.mp3"_s));
    EXPECT_EQ(matchPath(index, path, 261).collection.tier, Tier::Exact);
}

TEST(Matcher, PastDownloadsMatchOnSizeAndName) // T20, T21
{
    const auto index = indexOf({}, {HistoryEntry{11, u"alice"_s, u"Music\\Bicep\\Glue.mp3"_s, 1234},
                                    HistoryEntry{12, u"carol"_s, u"Share\\Other.mp3"_s, 999}});
    const MatchResult other = matchPath(index, u"Elsewhere\\GLUE.mp3"_s, 269, {}, 1234, {}, u"bob"_s);
    EXPECT_EQ(other.history, QList<quint64>{11});
    const MatchResult samePath = matchPath(index, u"Share\\Other.mp3"_s, 100, {}, 5555, {}, u"carol"_s);
    EXPECT_EQ(samePath.history, QList<quint64>{12});
    const MatchResult differentSize = matchPath(index, u"Elsewhere\\Glue.mp3"_s, 269, {}, 1235, {}, u"bob"_s);
    EXPECT_TRUE(differentSize.history.isEmpty());
}

TEST(Matcher, MixesOnlyCheckHistory) // T32
{
    const auto index = indexOf({O{1, u"Artist"_s, u"Live @ Venue"_s, 3720000}},
                               {HistoryEntry{5, u"p"_s, u"Mixes\\Artist - Live @ Venue.mp3"_s, 100}});
    const MatchResult result = matchPath(index, u"Mixes\\Artist - Live @ Venue.mp3"_s, 3720, {}, 100);
    EXPECT_TRUE(result.mix);
    EXPECT_FALSE(result.collection.isSet());
    EXPECT_EQ(result.history, QList<quint64>{5});
}

TEST(Matcher, UpgradeNeedsEstablishedQuality) // T33, LIB-4
{
    const auto index =
        indexOf({O{1, u"Artist"_s, u"Title"_s, 300000, {}, Source::Collection, {Codec::Mp3, 192, false, 44100, 0}}});
    RemoteAttributes lossless;
    lossless.sampleRateHz = 44100;
    lossless.bitDepth = 16;
    const MatchResult flac = matchPath(index, u"Artist - Title.flac"_s, 300, {}, 30'000'000, lossless);
    EXPECT_EQ(flac.collection.tier, Tier::Exact);
    EXPECT_EQ(flac.collection.relation, Relation::Upgrade);

    RemoteAttributes claimed;
    claimed.bitrateKbps = 320;
    const MatchResult fake = matchPath(index, u"Artist - Title.mp3"_s, 300, {}, 4'800'000, claimed); // ≈ 128 kbps
    EXPECT_EQ(fake.collection.tier, Tier::Exact);
    EXPECT_EQ(fake.collection.relation, Relation::Same);

    const MatchResult real = matchPath(index, u"Artist - Title.mp3"_s, 300, {}, 12'100'000, claimed);
    EXPECT_EQ(real.collection.relation, Relation::Upgrade);
}

TEST(Matcher, InboxAndCollectionAreSeparate) // SRCH-11 badges
{
    const auto index = indexOf({O{1, u"Bicep"_s, u"Glue"_s, 269000, {}, Source::Inbox},
                                O{2, u"Bicep"_s, u"Apricots"_s, 330000, {}, Source::Collection}});
    const MatchResult result = matchPath(index, u"Bicep - Glue.mp3"_s, 269);
    EXPECT_FALSE(result.collection.isSet());
    EXPECT_EQ(result.inbox.tier, Tier::Exact);
    EXPECT_EQ(result.inbox.id, 1U);
    EXPECT_EQ(result.inbox.source, Source::Inbox);
}

TEST(Matcher, TwoFittingVersionsCapAGuessAtLikely) // note §9 ambiguity rule
{
    // An unversioned remote fits the owned Original (probably the same) and the owned Extended (lengths agree): which
    // one it is would be a guess.
    const auto index = indexOf(
        {O{1, u"Artist"_s, u"Title (Original Mix)"_s, 300000}, O{2, u"Artist"_s, u"Title (Extended Mix)"_s, 302000}});
    EXPECT_EQ(matchPath(index, u"Artist - Title.mp3"_s, 300).collection.tier, Tier::Likely);
}

TEST(Matcher, TheNamedVersionWinsOverAnUnversionedTwin)
{
    // The remote names its version and we own exactly that version: exact, and that record, even though an
    // unversioned copy of the same length is owned too.
    const auto index =
        indexOf({O{1, u"Artist"_s, u"Title"_s, 300000}, O{2, u"Artist"_s, u"Title (Original Mix)"_s, 300000}});
    const MatchResult result = matchPath(index, u"Artist - Title (Original Mix).mp3"_s, 300);
    EXPECT_EQ(result.collection.tier, Tier::Exact);
    EXPECT_EQ(result.collection.id, 2U);
    // An unversioned remote matches the unversioned copy exactly.
    const MatchResult plain = matchPath(index, u"Artist - Title.mp3"_s, 300);
    EXPECT_EQ(plain.collection.tier, Tier::Exact);
    EXPECT_EQ(plain.collection.id, 1U);
}

TEST(Matcher, ExplanationsDoNotChangeDecisions)
{
    const auto index =
        indexOf({O{1, u"Bicep"_s, u"Glue"_s, 269000}, O{2, u"Bicep"_s, u"Glue (Extended Mix)"_s, 392000}});
    const ParsedRemote parsed =
        RemotePathParser::parse(u"Bicp - Glue.mp3"_s, {}, ParseContext{&index->artists(), nullptr});
    RemoteFile file;
    file.path = u"Bicp - Glue.mp3"_s;
    file.attributes.durationSec = 269;
    const MatchResult plain = Matcher(index).match(parsed, file);
    const MatchResult explained = Matcher(index).match(parsed, file, MatchOptions{true});
    EXPECT_TRUE(plain.explanation.isEmpty());
    EXPECT_FALSE(explained.explanation.isEmpty());
    EXPECT_EQ(plain.collection.tier, explained.collection.tier);
    EXPECT_EQ(plain.collection.id, explained.collection.id);
    EXPECT_DOUBLE_EQ(plain.collection.scores.score, explained.collection.scores.score);
}

TEST(Matcher, NullIndexMatchesNothing)
{
    const Matcher matcher(nullptr);
    RemoteFile file;
    file.path = u"Bicep - Glue.mp3"_s;
    const MatchResult result = matcher.match(RemotePathParser::parse(file.path), file, MatchOptions{true});
    EXPECT_FALSE(result.collection.isSet());
    EXPECT_FALSE(result.explanation.isEmpty());
    EXPECT_TRUE(matcher.matchLocal(LocalTrackQuery{}).isEmpty());
}

TEST(Matcher, LocalTracksFindSinglesOutsideTheirAlbum) // supersede candidates, DL-11
{
    MatchIndexInput input;
    LibraryTrack single = track(O{1, u"Bicep"_s, u"Glue"_s, 269000, {}, Source::Inbox});
    LibraryTrack onAlbum = track(O{2, u"Bicep"_s, u"Glue"_s, 269500, u"Isles"_s});
    onAlbum.albumId = 50;
    input.tracks = {single, onAlbum};
    const auto index = MatchIndex::build(input);
    LocalTrackQuery query;
    query.artist = u"Bicep"_s;
    query.title = u"Glue"_s;
    query.durationMs = 269400;
    query.excludeAlbumId = 50;
    const QList<selecta::match::RecordMatch> matches = Matcher(index).matchLocal(query);
    ASSERT_EQ(matches.size(), 1);
    EXPECT_EQ(matches.front().id, 1U);
    EXPECT_EQ(matches.front().tier, Tier::Exact);
    EXPECT_EQ(Matcher(index).matchLocal(query, 0).size(), 0);
}

TEST(MatchIndex, ReportsItsContents)
{
    MatchIndexInput input;
    input.tracks = {track(O{1, u"Bicep"_s, u"Glue"_s, 269000}), track(O{2, u"Bicep"_s, u"Apricots"_s, 330000})};
    LibraryTrack history = track(O{3, u"X"_s, u"Y"_s, 1000});
    history.source = Source::History; // not valid for tracks: dropped
    input.tracks.append(history);
    input.history = {HistoryEntry{1, u"p"_s, u"a\\b.mp3"_s, 10}};
    input.generation = 42;
    const auto index = MatchIndex::build(input);
    EXPECT_EQ(index->generation(), 42U);
    EXPECT_EQ(index->trackCount(), 2);
    EXPECT_EQ(index->historyCount(), 1);
    EXPECT_EQ(index->droppedRecords(), 1);
    EXPECT_EQ(index->normVersion(), selecta::match::kNormVersion);
    EXPECT_GT(index->vocabularySize(), 0);
    EXPECT_GT(index->memoryUsage(), 0U);
    EXPECT_TRUE(index->artists().lookup(u"Bicep"_s).exact());
    const auto empty = MatchIndex::build({});
    EXPECT_EQ(empty->trackCount(), 0);
}

} // namespace
