// SPDX-License-Identifier: GPL-3.0-or-later
// Remote path parsing (note §4.1, §17.5 A): the edge-case table's parsing rows and Sockseek's seed list
// (re-expressed, not copied).
#include "match/RemotePathParser.h"

#include "MatchTestSupport.h"
#include "match/ArtistDictionary.h"

#include <gtest/gtest.h>
#include <optional>

using namespace Qt::StringLiterals;
using selecta::match::ArtistDictionary;
using selecta::match::FileKind;
using selecta::match::ParseContext;
using selecta::match::ParsedRemote;
using selecta::match::ParseRule;
using selecta::match::RemotePathParser;
using selecta::match::VersionClass;

namespace {

struct ParseCase
{
    const char *name = "";
    QString path;
    QStringList siblings;
    QStringList knownArtists; // the collection's artist dictionary
    QString artist;
    QString title;
    QString album;
    QString albumArtist;
    int year = 0;
    int disc = 0;
    int track = 0;
    QString vinyl;
    QString catalogue;
    QString source;
    QString quality;
    bool variousArtists = false;
    bool unparseable = false;
    bool ambiguous = false;
    bool wholeNameOnly = false;
    std::optional<VersionClass> version;
    std::optional<ParseRule> rule; // a rule that must have fired
};

void PrintTo(const ParseCase &c, std::ostream *os)
{
    *os << c.name;
}

class ParseTest : public testing::TestWithParam<ParseCase>
{};

TEST_P(ParseTest, ParsesTheFields)
{
    const ParseCase &c = GetParam();
    QList<ArtistDictionary::Entry> entries;
    for (const QString &artist : c.knownArtists) {
        entries.append({artist, 1});
    }
    const ArtistDictionary dictionary(entries);
    const ParsedRemote parsed = RemotePathParser::parse(c.path, c.siblings, ParseContext{&dictionary, nullptr});

    EXPECT_EQ(parsed.kind, FileKind::Audio);
    EXPECT_EQ(parsed.artist.value, c.artist);
    EXPECT_EQ(parsed.title.value, c.title);
    EXPECT_EQ(parsed.album.value, c.album);
    if (!c.albumArtist.isNull()) {
        EXPECT_EQ(parsed.albumArtist.value, c.albumArtist);
    }
    EXPECT_EQ(parsed.year.value, c.year);
    EXPECT_EQ(parsed.disc.value, c.disc);
    EXPECT_EQ(parsed.track.value, c.track);
    EXPECT_EQ(parsed.vinylPosition, c.vinyl);
    EXPECT_EQ(parsed.catalogue, c.catalogue);
    EXPECT_EQ(parsed.sourceHint, c.source);
    if (!c.quality.isNull()) {
        EXPECT_EQ(parsed.qualityHint, c.quality);
    }
    EXPECT_EQ(parsed.variousArtists, c.variousArtists);
    EXPECT_EQ(parsed.unparseable, c.unparseable);
    EXPECT_EQ(parsed.orderAmbiguous, c.ambiguous);
    EXPECT_EQ(parsed.wholeNameOnly, c.wholeNameOnly);
    if (c.version) {
        EXPECT_EQ(parsed.version.versionClass, *c.version);
    }
    if (c.rule) {
        EXPECT_TRUE(parsed.rules.contains(*c.rule)) << "rule " << static_cast<int>(*c.rule) << " did not fire";
    }
    // Every set field carries a confidence and the rule that set it.
    for (const auto *field : {&parsed.artist, &parsed.title, &parsed.album}) {
        if (field->isSet()) {
            EXPECT_GT(field->confidence, 0.0F);
            EXPECT_LE(field->confidence, 1.0F);
        }
    }
}

QStringList numbered(const QString &pattern, int from, int to)
{
    QStringList names;
    for (int i = from; i <= to; ++i) {
        names.append(pattern.arg(i, 2, 10, QChar(u'0')));
    }
    return names;
}

INSTANTIATE_TEST_SUITE_P(
    EdgeCases, ParseTest,
    testing::Values(
        ParseCase{.name = "T01_ArtistAlbumFolders",
                  .path = u"Music\\Daft Punk\\Discovery (2001)\\03 - Digital Love.flac"_s,
                  .artist = u"Daft Punk"_s,
                  .title = u"Digital Love"_s,
                  .album = u"Discovery"_s,
                  .albumArtist = u"Daft Punk"_s,
                  .year = 2001,
                  .track = 3,
                  .rule = ParseRule::ArtistFolder},
        ParseCase{.name = "T02_BeatportSingle",
                  .path = u"DJ\\Bicep - Glue (Original Mix).mp3"_s,
                  .artist = u"Bicep"_s,
                  .title = u"Glue (Original Mix)"_s,
                  .version = VersionClass::Original},
        ParseCase{.name = "T06_SiblingConstantSwapsOrder",
                  .path = u"Folder\\01 - Alpha - Artist.mp3"_s,
                  .siblings = {u"02 - Beta - Artist.mp3"_s, u"03 - Gamma - Artist.mp3"_s, u"04 - Delta - Artist.mp3"_s},
                  .artist = u"Artist"_s,
                  .title = u"Alpha"_s,
                  .album = u"Folder"_s,
                  .track = 1,
                  .rule = ParseRule::OrderSiblingConstant},
        ParseCase{
            .name = "T07_HyphenInArtist", .path = u"Jay-Z - Song.mp3"_s, .artist = u"Jay-Z"_s, .title = u"Song"_s},
        ParseCase{.name = "T08_Scene",
                  .path = u"mp3\\Artist-Album-WEB-2019-GRP\\01-artist-title_name.mp3"_s,
                  .artist = u"artist"_s,
                  .title = u"title name"_s,
                  .album = u"Album"_s,
                  .year = 2019,
                  .track = 1,
                  .source = u"WEB"_s,
                  .rule = ParseRule::SceneFolder},
        ParseCase{.name = "T10_NumericTitleKept",
                  .path = u"Prince\\1999 (1982)\\01 1999.flac"_s,
                  .siblings = {u"02 Little Red Corvette.flac"_s, u"03 Delirious.flac"_s},
                  .artist = u"Prince"_s,
                  .title = u"1999"_s,
                  .album = u"1999"_s,
                  .year = 1982,
                  .track = 1},
        ParseCase{.name = "T11_LeadingNumberKept",
                  .path = u"808 State - Pacific State.mp3"_s,
                  .artist = u"808 State"_s,
                  .title = u"Pacific State"_s},
        ParseCase{.name = "T19_UntaggedRip",
                  .path = u"Rips\\Track 01.wav"_s,
                  .track = 1,
                  .unparseable = true,
                  .rule = ParseRule::GenericRipName},
        ParseCase{.name = "T22_VariousArtists",
                  .path = u"VA - Summer 2020 (2020)\\03 - Artist - Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .album = u"Summer 2020"_s,
                  .year = 2020,
                  .track = 3,
                  .variousArtists = true},
        ParseCase{.name = "T23_DiscFolder",
                  .path = u"Artist - Album (2010)\\CD2\\05 - Title.flac"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .album = u"Album"_s,
                  .albumArtist = u"Artist"_s,
                  .year = 2010,
                  .disc = 2,
                  .track = 5,
                  .rule = ParseRule::DiscFolder},
        ParseCase{.name = "T24_ITunesCompilation",
                  .path = u"iTunes Media\\Music\\Compilations\\Summer Hits\\1-05 Title.m4a"_s,
                  .title = u"Title"_s,
                  .album = u"Summer Hits"_s,
                  .disc = 1,
                  .track = 5,
                  .variousArtists = true,
                  .wholeNameOnly = true},
        ParseCase{.name = "T25_Vinyl",
                  .path = u"Artist - Album [LP]\\B2 Title.flac"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .album = u"Album"_s,
                  .vinyl = u"B2"_s,
                  .source = u"VINYL"_s},
        ParseCase{.name = "T26_WebJunk",
                  .path = u"www.site.ru - Artist - Title (Official Video) [HQ].mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .rule = ParseRule::WebsiteStripped},
        ParseCase{.name = "T27_Catalogue",
                  .path = u"[DC123] Artist - Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .catalogue = u"DC123"_s},
        ParseCase{.name = "T43_Bandcamp",
                  .path = u"Artist - Album\\Artist - Album - 01 Title.flac"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .album = u"Album"_s,
                  .track = 1,
                  .rule = ParseRule::ArtistAlbumTitle},
        ParseCase{.name = "T44_TrailingVersionSegment",
                  .path = u"Artist - Song - Extended Mix.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Song - Extended Mix"_s,
                  .version = VersionClass::Extended},
        ParseCase{.name = "T47_Mojibake",
                  .path = u"RÃ¶yksopp - Eple.mp3"_s,
                  .artist = u"Röyksopp"_s,
                  .title = u"Eple"_s,
                  .rule = ParseRule::MojibakeRepaired},
        ParseCase{.name = "T48_NumbersOnly",
                  .path = u"Неизвестный альбом\\01.flac"_s,
                  .siblings = numbered(u"%1.flac"_s, 2, 10),
                  .album = u"Неизвестный альбом"_s,
                  .track = 1,
                  .unparseable = true,
                  .rule = ParseRule::NumberOnlyName},
        ParseCase{.name = "T70_KnownArtistSwapsOrder",
                  .path = u"Glue - Bicep.mp3"_s,
                  .knownArtists = {u"Bicep"_s},
                  .artist = u"Bicep"_s,
                  .title = u"Glue"_s,
                  .rule = ParseRule::OrderKnownArtist},
        ParseCase{.name = "T70_WithoutDictionaryThePriorDecides",
                  .path = u"Glue - Bicep.mp3"_s,
                  .artist = u"Glue"_s,
                  .title = u"Bicep"_s,
                  .rule = ParseRule::OrderPrior},
        ParseCase{.name = "T71_ArtistAlbumTitleFolders",
                  .path = u"Bicep\\Isles (2021)\\03 Apricots.flac"_s,
                  .artist = u"Bicep"_s,
                  .title = u"Apricots"_s,
                  .album = u"Isles"_s,
                  .year = 2021,
                  .track = 3},
        ParseCase{.name = "T72_ReversedFolder",
                  .path = u"Isles - Bicep\\03 - Apricots.flac"_s,
                  .knownArtists = {u"Bicep"_s},
                  .artist = u"Bicep"_s,
                  .title = u"Apricots"_s,
                  .album = u"Isles"_s,
                  .albumArtist = u"Bicep"_s,
                  .track = 3,
                  .rule = ParseRule::AlbumArtistFolder},
        ParseCase{.name = "MemberOnlyEvidenceIsAmbiguous",
                  .path = u"Glue - Bicep.mp3"_s,
                  .knownArtists = {u"Bicep & Hammer"_s},
                  .artist = u"Bicep"_s,
                  .title = u"Glue"_s,
                  .ambiguous = true}),
    [](const testing::TestParamInfo<ParseCase> &info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    SeedList, ParseTest,
    testing::Values(
        ParseCase{.name = "HundredsWithoutSiblingsStayInTheName",
                  .path = u"101 Artist - Title.mp3"_s,
                  .artist = u"101 Artist"_s,
                  .title = u"Title"_s},
        ParseCase{.name = "HundredsStrippedByTheDictionary",
                  .path = u"101 Artist - Title.mp3"_s,
                  .knownArtists = {u"Artist"_s},
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .track = 101},
        ParseCase{.name = "MidNameTrackNumber",
                  .path = u"Artist - 01 Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .track = 1,
                  .rule = ParseRule::MidTrackNumber},
        ParseCase{.name = "NumericTitleSegment_T62", .path = u"MK - 17.mp3"_s, .artist = u"MK"_s, .title = u"17"_s},
        ParseCase{.name = "MiddleTrackNumberSegment",
                  .path = u"Artist - 03 - Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .track = 3,
                  .rule = ParseRule::MidTrackNumber},
        ParseCase{.name = "DiscTrackPrefix",
                  .path = u"1-01 Artist - Song.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Song"_s,
                  .disc = 1,
                  .track = 1},
        ParseCase{.name = "Underscores",
                  .path = u"Some_Artist - Some_Title.mp3"_s,
                  .artist = u"Some Artist"_s,
                  .title = u"Some Title"_s},
        ParseCase{.name = "EmDash", .path = u"Artist — Title.mp3"_s, .artist = u"Artist"_s, .title = u"Title"_s},
        ParseCase{.name = "LeadingGroupUnwrapped",
                  .path = u"(Daft Punk - One More Time) Robin Schulz Remix.mp3"_s,
                  .artist = u"Daft Punk"_s,
                  .title = u"One More Time (Robin Schulz Remix)"_s,
                  .version = VersionClass::Remix},
        ParseCase{.name = "TitleOpeningWithBrackets", // live test 2026-10-20
                  .path = u"Peggy Gou - (It Goes Like) Nanana.flac"_s,
                  .artist = u"Peggy Gou"_s,
                  .title = u"(It Goes Like) Nanana"_s},
        ParseCase{.name = "TitleOnlyOpeningWithBrackets",
                  .path = u"Peggy Gou\\(It Goes Like) Nanana.flac"_s,
                  .title = u"(It Goes Like) Nanana"_s,
                  .album = u"Peggy Gou"_s,
                  .wholeNameOnly = true},
        ParseCase{.name = "LeadingGroupBeforeArtistAndTitleStaysOut",
                  .path = u"(Lantern Grove) Kestrel Moon - Glass Harbour.mp3"_s,
                  .artist = u"Kestrel Moon"_s,
                  .title = u"Glass Harbour"_s},
        ParseCase{.name = "QualityTag",
                  .path = u"Artist - Title (Original Mix) [320].mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title (Original Mix)"_s,
                  .quality = u"320"_s},
        ParseCase{.name = "BitDepthAndRate",
                  .path = u"Artist - Title [FLAC 24-96].flac"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s,
                  .quality = u"FLAC 24-96"_s},
        ParseCase{.name = "NumberedBand",
                  .path = u"2 Unlimited - Get Ready For This.mp3"_s,
                  .artist = u"2 Unlimited"_s,
                  .title = u"Get Ready For This"_s},
        ParseCase{.name = "PunctuatedTrackNumber",
                  .path = u"03. Title.mp3"_s,
                  .title = u"Title"_s,
                  .track = 3,
                  .wholeNameOnly = true},
        ParseCase{.name = "NumericTitleAlone", .path = u"1999.mp3"_s, .title = u"1999"_s, .wholeNameOnly = true},
        ParseCase{.name = "BareHyphenNotSplitOutsideScene",
                  .path = u"music\\artist-title.mp3"_s,
                  .title = u"artist-title"_s,
                  .wholeNameOnly = true},
        ParseCase{.name = "ShareRootAndGenericFolders",
                  .path = u"@@abcde\\Music\\Artist - Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s},
        ParseCase{.name = "GenreAndYearFoldersAreNoise_L3",
                  .path = u"DJ\\Techno\\2024\\Artist - Title (Original Mix).mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title (Original Mix)"_s},
        ParseCase{.name = "DiscFolderWithTitle",
                  .path = u"Bicep\\Isles\\CD1 - Bonus\\01 Title.flac"_s,
                  .artist = u"Bicep"_s,
                  .title = u"Title"_s,
                  .album = u"Isles"_s,
                  .disc = 1,
                  .track = 1},
        ParseCase{.name = "ForwardSlashes",
                  .path = u"Music/Bicep/Isles/03 Apricots.flac"_s,
                  .artist = u"Bicep"_s,
                  .title = u"Apricots"_s,
                  .album = u"Isles"_s,
                  .track = 3},
        ParseCase{.name = "FeaturedArtistInName",
                  .path = u"Artist feat. Guest - Title.mp3"_s,
                  .artist = u"Artist"_s,
                  .title = u"Title"_s}),
    [](const testing::TestParamInfo<ParseCase> &info) { return std::string(info.param.name); });

TEST(RemotePathParser, HundredsEncodeDiscs) // T49
{
    QStringList files;
    for (int i = 1; i <= 12; ++i) {
        files.append(u"%1 Song %2.flac"_s.arg(100 + i).arg(i));
    }
    for (int i = 1; i <= 9; ++i) {
        files.append(u"%1 Other %2.flac"_s.arg(200 + i).arg(i));
    }
    const QList<ParsedRemote> parsed = RemotePathParser::parseFolder(u"Artist - Album"_s, files);
    ASSERT_EQ(parsed.size(), 21);
    EXPECT_EQ(parsed.at(0).disc.value, 1);
    EXPECT_EQ(parsed.at(0).track.value, 1);
    EXPECT_EQ(parsed.at(11).disc.value, 1);
    EXPECT_EQ(parsed.at(11).track.value, 12);
    EXPECT_EQ(parsed.at(12).disc.value, 2);
    EXPECT_EQ(parsed.at(12).track.value, 1);
    EXPECT_EQ(parsed.at(20).track.value, 9);
    EXPECT_EQ(parsed.at(12).title.value, u"Other 1"_s);
    EXPECT_TRUE(parsed.at(0).rules.contains(ParseRule::HundredsDiscTrack));
}

TEST(RemotePathParser, FolderOfVariousArtistsIsDetectedFromTheFiles) // note §8
{
    const QStringList files{u"01 - Alpha - One.mp3"_s, u"02 - Beta - Two.mp3"_s, u"03 - Gamma - Three.mp3"_s,
                            u"04 - Delta - Four.mp3"_s};
    const QList<ParsedRemote> parsed = RemotePathParser::parseFolder(u"Summer Hits"_s, files);
    ASSERT_EQ(parsed.size(), 4);
    for (const ParsedRemote &file : parsed) {
        EXPECT_TRUE(file.variousArtists);
        EXPECT_TRUE(file.rules.contains(ParseRule::VariousArtistsFiles));
    }
    EXPECT_EQ(parsed.at(1).artist.value, u"Beta"_s);
    EXPECT_EQ(parsed.at(1).title.value, u"Two"_s);
}

TEST(RemotePathParser, ParseFolderEqualsParseWithSiblings)
{
    const QStringList files{u"01 - Alpha - Artist.mp3"_s, u"02 - Beta - Artist.mp3"_s, u"03 - Gamma - Artist.mp3"_s};
    const QList<ParsedRemote> folder = RemotePathParser::parseFolder(u"Music\\Album"_s, files);
    const ParsedRemote single = RemotePathParser::parse(u"Music\\Album\\02 - Beta - Artist.mp3"_s, files);
    EXPECT_EQ(folder.at(1).artist.value, single.artist.value);
    EXPECT_EQ(folder.at(1).title.value, single.title.value);
    EXPECT_EQ(folder.at(1).keys.exactKey, single.keys.exactKey);
    EXPECT_EQ(single.path, u"Music\\Album\\02 - Beta - Artist.mp3"_s);
}

TEST(RemotePathParser, ZonesFeedTheReferenceScorer) // note §17.5 B
{
    const ParsedRemote parsed =
        RemotePathParser::parse(u"Share\\Bicep\\Isles (2021)\\03 Apricots (Bicep Remix) [FLAC].flac"_s);
    EXPECT_EQ(parsed.zones.body, u"Apricots"_s);
    EXPECT_EQ(parsed.zones.brackets, QStringList{u"Bicep Remix"_s});
    EXPECT_EQ(parsed.zones.parent, u"Isles"_s);
    EXPECT_EQ(parsed.zones.higher, QStringList{u"Bicep"_s});
    EXPECT_EQ(parsed.version.versionClass, VersionClass::Remix);
    EXPECT_EQ(parsed.qualityHint, u"FLAC"_s);
}

TEST(RemotePathParser, KeysAreComputed)
{
    const ParsedRemote parsed = RemotePathParser::parse(u"AC_DC - Back In Black.mp3"_s); // T28
    EXPECT_EQ(parsed.artist.value, u"AC_DC"_s);
    EXPECT_EQ(parsed.keys.artist.field.squashed, u"acdc"_s);
    EXPECT_EQ(parsed.keys.title.field.key, u"back in black"_s);
}

TEST(RemotePathParser, NonAudioFiles) // note §14
{
    const ParsedRemote cover = RemotePathParser::parse(u"Artist - Album\\cover.jpg"_s);
    EXPECT_EQ(cover.kind, FileKind::Companion);
    EXPECT_TRUE(cover.unparseable);
    EXPECT_EQ(cover.album.value, u"Album"_s);
    EXPECT_EQ(RemotePathParser::parse(u"Artist - Album\\setup.exe"_s).kind, FileKind::Other);
    EXPECT_EQ(RemotePathParser::parse(u"Artist - Album\\README"_s).kind, FileKind::Other);
    EXPECT_EQ(RemotePathParser::fileKind(u"FLAC"_s), FileKind::Audio);
    EXPECT_EQ(RemotePathParser::fileKind(u"cue"_s), FileKind::Companion);
}

TEST(RemotePathParser, BoundsHostileInput) // untrusted input
{
    QString longPath;
    for (int i = 0; i < 2000; ++i) {
        longPath += u"Folder%1\\"_s.arg(i);
    }
    longPath += u"Artist - Title.mp3"_s;
    const ParsedRemote parsed = RemotePathParser::parse(longPath);
    EXPECT_TRUE(parsed.truncated);
    EXPECT_TRUE(parsed.rules.contains(ParseRule::PathTruncated));
    EXPECT_EQ(parsed.artist.value, u"Artist"_s);
    EXPECT_LE(parsed.path.size(), RemotePathParser::kMaxPathLength);

    QStringList many;
    for (int i = 0; i < 5000; ++i) {
        many.append(u"%1 - Artist - Song %1.mp3"_s.arg(i));
    }
    const QList<ParsedRemote> folder = RemotePathParser::parseFolder(u"Big"_s, many);
    EXPECT_EQ(folder.size(), many.size());

    EXPECT_TRUE(RemotePathParser::parse(QString()).unparseable);
    EXPECT_TRUE(RemotePathParser::parse(u"\\\\\\"_s).unparseable);
    EXPECT_TRUE(RemotePathParser::parse(u"((((((.mp3"_s).unparseable);
    EXPECT_TRUE(RemotePathParser::parseFolder(u"x"_s, {}).isEmpty());
}

} // namespace
