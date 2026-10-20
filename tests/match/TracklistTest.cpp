// SPDX-License-Identifier: GPL-3.0-or-later
// Get tracks line parsing (PRD BATCH-1, note §9.1, T50): the "Artist - Title" variants pasted in the wild.
#include "match/Tracklist.h"

#include "MatchTestSupport.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::TracklistLine;
using selecta::match::TracklistLineKind;
using selecta::match::TracklistParser;
using selecta::match::VersionClass;

namespace {

struct LineCase
{
    const char *name = "";
    QString line;
    TracklistLineKind kind = TracklistLineKind::Track;
    QString artist;
    QString title;
    QString label;
    int timestamp = -1;
    int duration = -1;
    bool mashup = false;
};

void PrintTo(const LineCase &c, std::ostream *os)
{
    *os << c.name;
}

class TracklistLineTest : public testing::TestWithParam<LineCase>
{};

TEST_P(TracklistLineTest, ParsesTheLine)
{
    const LineCase &c = GetParam();
    const TracklistLine parsed = TracklistParser::parseLine(c.line);
    EXPECT_EQ(parsed.kind, c.kind);
    EXPECT_EQ(parsed.artist, c.artist);
    EXPECT_EQ(parsed.title, c.title);
    EXPECT_EQ(parsed.labelHint, c.label);
    EXPECT_EQ(parsed.timestampSec, c.timestamp);
    EXPECT_EQ(parsed.durationSec, c.duration);
    EXPECT_EQ(parsed.mashup, c.mashup);
    EXPECT_EQ(parsed.raw, c.line.first(std::min<qsizetype>(c.line.size(), 512)));
}

using K = TracklistLineKind;

INSTANTIATE_TEST_SUITE_P(
    T50, TracklistLineTest,
    testing::Values(LineCase{.name = "IdId", .line = u"ID - ID"_s, .kind = K::Unidentified, .title = u"ID"_s},
                    LineCase{.name = "ArtistId",
                             .line = u"Artist - ID"_s,
                             .kind = K::Unidentified,
                             .artist = u"Artist"_s,
                             .title = u"ID"_s},
                    LineCase{.name = "TimestampAndLabel",
                             .line = u"12:34 Artist - Title [LABEL]"_s,
                             .artist = u"Artist"_s,
                             .title = u"Title"_s,
                             .label = u"LABEL"_s,
                             .timestamp = 754}),
    [](const testing::TestParamInfo<LineCase> &info) { return std::string(info.param.name); });

INSTANTIATE_TEST_SUITE_P(
    Variants, TracklistLineTest,
    testing::Values(
        LineCase{.name = "Plain", .line = u"Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "EnDash", .line = u"Bicep – Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "EmDash", .line = u"Bicep — Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "DoubleHyphen", .line = u"Bicep -- Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "HyphenInArtist",
                 .line = u"Jay-Z - 99 Problems"_s,
                 .artist = u"Jay-Z"_s,
                 .title = u"99 Problems"_s},
        LineCase{.name = "VersionKept",
                 .line = u"Bicep - Glue (Original Mix)"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue (Original Mix)"_s},
        LineCase{.name = "VersionThenLabel",
                 .line = u"Bicep - Glue (Original Mix) [Ninja Tune]"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue (Original Mix)"_s,
                 .label = u"Ninja Tune"_s},
        LineCase{.name = "DotNumbering", .line = u"01. Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "ParenNumbering", .line = u"1) Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "HashNumbering", .line = u"#1 Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "BracketNumbering", .line = u"[01] Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{
            .name = "LeadingZeroNumbering", .line = u"03 Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "DashNumbering", .line = u"1 - Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "TrackWord", .line = u"Track 3: Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "Bullet", .line = u"• Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "DashBullet", .line = u"- Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "BracketTimestamp",
                 .line = u"[1:02:03] Bicep - Glue"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue"_s,
                 .timestamp = 3723},
        LineCase{.name = "NumberThenTimestamp",
                 .line = u"02. (05:10) Bicep - Glue"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue"_s,
                 .timestamp = 310},
        LineCase{.name = "TrailingLength",
                 .line = u"1. Bicep - Glue 4:29"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue"_s,
                 .duration = 269},
        LineCase{.name = "TrailingLengthInParens",
                 .line = u"Bicep - Glue (4:29)"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue"_s,
                 .duration = 269},
        LineCase{.name = "VinylPosition", .line = u"A1 Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "BandCalledA1",
                 .line = u"A1 - Caught in the Middle"_s,
                 .artist = u"A1"_s,
                 .title = u"Caught in the Middle"_s},
        LineCase{.name = "NumberedBandKept",
                 .line = u"808 State - Pacific State"_s,
                 .artist = u"808 State"_s,
                 .title = u"Pacific State"_s},
        LineCase{.name = "Tabs", .line = u"Bicep\tGlue\tIsles"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "QuotedTitle", .line = u"Bicep \"Glue\""_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "CurlyQuotedTitle", .line = u"Bicep – “Glue”"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "FreeText", .line = u"bicep glue"_s, .title = u"bicep glue"_s},
        LineCase{.name = "MashupInline",
                 .line = u"Bicep - Glue w/ Daft Punk - Da Funk"_s,
                 .artist = u"Bicep"_s,
                 .title = u"Glue w/ Daft Punk - Da Funk"_s,
                 .mashup = true},
        LineCase{.name = "MashupLayer",
                 .line = u"w/ Daft Punk - Da Funk"_s,
                 .artist = u"Daft Punk"_s,
                 .title = u"Da Funk"_s,
                 .mashup = true},
        LineCase{.name = "UnknownArtist",
                 .line = u"Unknown Artist - Unknown Title"_s,
                 .kind = K::Unidentified,
                 .title = u"Unknown Title"_s},
        LineCase{.name = "IdWithVersion",
                 .line = u"Artist - ID (Remix)"_s,
                 .kind = K::Unidentified,
                 .artist = u"Artist"_s,
                 .title = u"ID (Remix)"_s},
        LineCase{.name = "IdArtistKnownTitle", .line = u"ID - Glue"_s, .title = u"Glue"_s},
        LineCase{.name = "WindowsLineEnding", .line = u"Bicep - Glue\r"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "NoBreakSpaces", .line = u"Bicep - Glue"_s, .artist = u"Bicep"_s, .title = u"Glue"_s},
        LineCase{.name = "Blank", .line = u"   "_s, .kind = K::Empty},
        LineCase{.name = "Heading", .line = u"Tracklist:"_s, .kind = K::Empty},
        LineCase{.name = "Separator", .line = u"----------"_s, .kind = K::Empty},
        LineCase{.name = "Link", .line = u"https://open.spotify.com/track/abc"_s, .kind = K::Empty},
        LineCase{.name = "OnlyANumber", .line = u"12."_s, .kind = K::Empty}),
    [](const testing::TestParamInfo<LineCase> &info) { return std::string(info.param.name); });

TEST(TracklistParser, KeysAndVersionsComeFromTheNormaliser)
{
    const TracklistLine line = TracklistParser::parseLine(u"Bicep - Glue (Hammer Remix)"_s);
    EXPECT_EQ(line.keys.title.version.versionClass, VersionClass::Remix);
    EXPECT_EQ(line.keys.title.version.by, QStringList{u"hammer"_s});
    EXPECT_EQ(line.keys.exactKey, u"bicep␟glue␟remix:hammer"_s);
}

TEST(TracklistParser, DeduplicatesAndSkipsEmptyLines) // BATCH-1
{
    const QList<TracklistLine> lines = TracklistParser::parse(u"Tracklist:\n"
                                                              u"01. Bicep - Glue\n"
                                                              u"\n"
                                                              u"02. BICEP - glue\r\n"
                                                              u"03. ID - ID\n"
                                                              u"04. ID - ID\n"
                                                              u"05. Daft Punk - Da Funk\n"_s);
    ASSERT_EQ(lines.size(), 3);
    EXPECT_EQ(lines.at(0).title, u"Glue"_s);
    EXPECT_EQ(lines.at(1).kind, TracklistLineKind::Unidentified);
    EXPECT_EQ(lines.at(2).artist, u"Daft Punk"_s);
}

TEST(TracklistParser, BareNumbersCountOnlyInASequence)
{
    const QList<TracklistLine> numbered =
        TracklistParser::parse(u"1 Bicep - Glue\n2 Daft Punk - Da Funk\n3 Moderat - Rusty Nails\n"_s);
    ASSERT_EQ(numbered.size(), 3);
    EXPECT_EQ(numbered.at(0).artist, u"Bicep"_s);
    EXPECT_EQ(numbered.at(2).artist, u"Moderat"_s);

    const QList<TracklistLine> bands =
        TracklistParser::parse(u"808 State - Pacific State\n50 Cent - In Da Club\nBicep - Glue\n"_s);
    ASSERT_EQ(bands.size(), 3);
    EXPECT_EQ(bands.at(0).artist, u"808 State"_s);
    EXPECT_EQ(bands.at(1).artist, u"50 Cent"_s);
}

TEST(TracklistParser, BoundsHugeInput)
{
    QString text;
    for (int i = 0; i < 6000; ++i) {
        text += u"%1. Artist %1 - Title %1\n"_s.arg(i + 1);
    }
    EXPECT_EQ(TracklistParser::parse(text).size(), TracklistParser::kMaxLines);
    const TracklistLine longLine = TracklistParser::parseLine(QString(5000, u'x'));
    EXPECT_EQ(longLine.raw.size(), 512);
    EXPECT_TRUE(TracklistParser::parse(QString()).isEmpty());
}

} // namespace
