// SPDX-License-Identifier: GPL-3.0-or-later
// Fit of a remote file to the chosen release (note §17.5 B, PRD SRCH-18, D23: length first).
#include "match/ReleaseScorer.h"

#include "MatchTestSupport.h"
#include "match/RemotePathParser.h"

#include <gtest/gtest.h>
#include <optional>

using namespace Qt::StringLiterals;
using selecta::match::ParsedRemote;
using selecta::match::ReleaseFit;
using selecta::match::ReleaseReference;
using selecta::match::ReleaseScorer;
using selecta::match::RemotePathParser;

namespace {

struct FitCase
{
    const char *name = "";
    QString path;
    int durationSec = 0;
    QStringList artists;
    QString title;
    qint64 releaseMs = 0;
    bool fits = false;
    std::optional<double> fit; // checked to 1e-6 when given
    bool lengthUnknown = false;
    bool artistOnlyInBrackets = false;
};

void PrintTo(const FitCase &c, std::ostream *os)
{
    *os << c.name;
}

class ReleaseFitTest : public testing::TestWithParam<FitCase>
{};

TEST_P(ReleaseFitTest, FitsTheRelease)
{
    const FitCase &c = GetParam();
    const ReleaseScorer scorer(ReleaseReference{c.artists, c.title, {}, c.releaseMs});
    const ParsedRemote parsed = RemotePathParser::parse(c.path);
    const ReleaseFit fit = scorer.score(parsed, c.durationSec, /*explain=*/true);
    EXPECT_EQ(fit.fits, c.fits) << fit.explanation.join(u'\n').toStdString();
    if (c.fit) {
        EXPECT_NEAR(fit.fit, *c.fit, 1e-6) << fit.explanation.join(u'\n').toStdString();
    }
    EXPECT_EQ(fit.lengthUnknown, c.lengthUnknown);
    EXPECT_EQ(fit.artistOnlyInBrackets, c.artistOnlyInBrackets);
    EXPECT_FALSE(fit.explanation.isEmpty());
}

INSTANTIATE_TEST_SUITE_P(EdgeCases, ReleaseFitTest,
                         testing::Values(FitCase{.name = "T53_JoinedArtist",
                                                 .path = u"Music\\okumadrift - lights.mp3"_s,
                                                 .durationSec = 240,
                                                 .artists = {u"Okuma Drift"_s},
                                                 .title = u"Lights"_s,
                                                 .releaseMs = 240000,
                                                 .fits = true,
                                                 .fit = 1.0},
                                         FitCase{.name = "T70_OrderDoesNotMatter",
                                                 .path = u"Glue - Bicep.mp3"_s,
                                                 .durationSec = 269,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Glue"_s,
                                                 .releaseMs = 269000,
                                                 .fits = true,
                                                 .fit = 1.0},
                                         FitCase{.name = "T71_ArtistInAFolder",
                                                 .path = u"Bicep\\Isles (2021)\\03 Apricots.flac"_s,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Apricots"_s,
                                                 .fits = true,
                                                 .fit = 1.0,
                                                 .lengthUnknown = true},
                                         FitCase{.name = "T73_ArtistOnlyAsRemixer",
                                                 .path = u"Other Artist - Glue (Bicep Remix).mp3"_s,
                                                 .durationSec = 269,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Glue"_s,
                                                 .releaseMs = 269000,
                                                 .fits = false,
                                                 .fit = 0.0,
                                                 .artistOnlyInBrackets = true},
                                         FitCase{.name = "T74_ReferenceArtistMissing",
                                                 .path = u"Bicep - Glue.mp3"_s,
                                                 .artists = {u"Other Artist"_s},
                                                 .title = u"Glue (Bicep Remix)"_s,
                                                 .fits = false,
                                                 .fit = 0.40 / 0.65,
                                                 .lengthUnknown = true},
                                         FitCase{.name = "T75_LongerTitleAndLength",
                                                 .path = u"Artist - Love Story.mp3"_s,
                                                 .durationSec = 235,
                                                 .artists = {u"Artist"_s},
                                                 .title = u"Love"_s,
                                                 .releaseMs = 200000,
                                                 .fits = false,
                                                 .fit = 0.65 * 0.5},
                                         FitCase{.name = "T76_GenericTitleRightLength",
                                                 .path = u"Moderat - Intro.mp3"_s,
                                                 .durationSec = 72,
                                                 .artists = {u"Moderat"_s},
                                                 .title = u"Intro"_s,
                                                 .releaseMs = 72000,
                                                 .fits = true,
                                                 .fit = 1.0},
                                         FitCase{.name = "T76_GenericTitleWrongLength",
                                                 .path = u"Moderat\\Other Album\\01 Intro.mp3"_s,
                                                 .durationSec = 125,
                                                 .artists = {u"Moderat"_s},
                                                 .title = u"Intro"_s,
                                                 .releaseMs = 72000,
                                                 .fits = false,
                                                 .fit = 0.65},
                                         FitCase{.name = "T87_TitleAndLengthCarryATypoedArtist",
                                                 .path = u"DJ Kose - Pick Up.mp3"_s,
                                                 .durationSec = 240,
                                                 .artists = {u"DJ Koze"_s},
                                                 .title = u"Pick Up"_s,
                                                 .releaseMs = 240000,
                                                 .fits = true,
                                                 .fit = 0.35 + 0.40 + (0.25 * 2.0 / 6.0)},
                                         FitCase{.name = "TruncatedLengthWithinWindow",
                                                 .path = u"Bicep - Glue.mp3"_s,
                                                 .durationSec = 266,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Glue"_s,
                                                 .releaseMs = 269900,
                                                 .fits = true},
                                         FitCase{.name = "LengthJustOutsideWindow",
                                                 .path = u"Bicep - Glue.mp3"_s,
                                                 .durationSec = 265,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Glue"_s,
                                                 .releaseMs = 269000,
                                                 .fits = false},
                                         // Live test 2026-10-20: "Check", not "Ready", for the file named exactly so.
                                         FitCase{
                                             .name = "TitleStartingWithBrackets",
                                             .path = u"flac_fiona\\Lossless\\Peggy Gou - (It Goes Like) Nanana.flac"_s,
                                             .durationSec = 368,
                                             .artists = {u"Peggy Gou"_s},
                                             .title = u"(It Goes Like) Nanana"_s,
                                             .releaseMs = 367613,
                                             .fits = true,
                                             .fit = 1.0},
                                         FitCase{.name = "ExtendedAgainstPlainReleaseDecidedByLength",
                                                 .path = u"Bicep - Glue (Extended Mix).mp3"_s,
                                                 .durationSec = 269,
                                                 .artists = {u"Bicep"_s},
                                                 .title = u"Glue"_s,
                                                 .releaseMs = 269000,
                                                 .fits = true}),
                         [](const testing::TestParamInfo<FitCase> &info) { return std::string(info.param.name); });

TEST(ReleaseScorer, OnlyThisReleaseFiltersAWideQuery) // T85
{
    const ReleaseScorer scorer(ReleaseReference{{u"Bicep"_s}, u"Glue"_s, {}, 269000});
    struct Result
    {
        QString path;
        int length = 0;
        bool expected = false;
    };
    const QList<Result> patterns{
        {u"Bicep - Glue.mp3"_s, 269, true},
        {u"Share\\Bicep\\Glue.flac"_s, 268, true},
        {u"Other - Glue.mp3"_s, 269, false},
        {u"Bicep - Glue.mp3"_s, 300, false},
        {u"Glue - Bicep.mp3"_s, 270, true},
        {u"Bicep - Glue (Remix).mp3"_s, 269, false},
        {u"Bicep - Glue Factory.mp3"_s, 269, false},
        {u"Glue.mp3"_s, 269, false},
        {u"Bicep - Glue (Original Mix).mp3"_s, 271, true},
        {u"Bicep - Glue.mp3"_s, 0, true},
    };
    int kept = 0;
    int expected = 0;
    for (int i = 0; i < 500; ++i) {
        const Result &result = patterns.at(i % patterns.size());
        const ReleaseFit fit = scorer.score(RemotePathParser::parse(result.path), result.length);
        ASSERT_EQ(fit.fits, result.expected) << result.path.toStdString() << " " << result.length;
        kept += fit.fits ? 1 : 0;
        expected += result.expected ? 1 : 0;
    }
    EXPECT_EQ(kept, expected);
}

TEST(ReleaseScorer, IdentityOrderPutsLengthFirst) // SRCH-18
{
    const ReleaseScorer scorer(ReleaseReference{{u"Bicep"_s}, u"Glue"_s, {}, 269000});
    const ReleaseFit exactLength = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 269);
    const ReleaseFit unknownLength = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 0);
    const ReleaseFit wrongLength = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 320);
    const ReleaseFit noArtist = scorer.score(RemotePathParser::parse(u"Glue.mp3"_s), 269);
    EXPECT_LT(ReleaseScorer::compareIdentity(exactLength, unknownLength), 0);
    EXPECT_LT(ReleaseScorer::compareIdentity(unknownLength, wrongLength), 0);
    EXPECT_LT(ReleaseScorer::compareIdentity(exactLength, noArtist), 0);
    EXPECT_EQ(ReleaseScorer::compareIdentity(exactLength, exactLength), 0);
    EXPECT_GT(ReleaseScorer::compareIdentity(wrongLength, exactLength), 0);
}

TEST(ReleaseScorer, AvailabilityOnlyBreaksTies) // Get tracks ranking
{
    const ReleaseScorer scorer(ReleaseReference{{u"Bicep"_s}, u"Glue"_s, {}, 269000});
    const ReleaseFit fits = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 269);
    const ReleaseFit misses = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 330);
    using selecta::match::RankedFile;
    const QList<RankedFile> files{
        {misses, 4, true, 0, u"fast"_s, u"a.mp3"_s}, // wrong length, free slot, best quality: last
        {fits, 2, true, 0, u"peer"_s, u"b.mp3"_s},   // fits, lower quality
        {fits, 4, false, 50, u"busy"_s, u"c.mp3"_s}, // fits, best quality, long queue: first
        {fits, 4, true, 3, u"free"_s, u"d.mp3"_s},   // fits, best quality, free slot: before c
        {fits, 4, true, 3, u"alpha"_s, u"e.mp3"_s},  // same as d: peer order decides
    };
    EXPECT_EQ(ReleaseScorer::rank(files), (QList<qsizetype>{4, 3, 2, 1, 0}));
    EXPECT_TRUE(ReleaseScorer::rank({}).isEmpty());
}

TEST(ReleaseScorer, CopiesShareThePreparedReference)
{
    const ReleaseScorer scorer(ReleaseReference{{u"Bicep"_s}, u"Glue"_s, {}, 269000});
    const ReleaseScorer copy = scorer; // NOLINT(performance-unnecessary-copy-initialization): the copy is the test
    EXPECT_EQ(copy.reference().title, u"Glue"_s);
    EXPECT_TRUE(copy.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 269).fits);
}

TEST(ReleaseScorer, EmptyReferenceFitsNothing)
{
    const ReleaseScorer scorer(ReleaseReference{});
    const ReleaseFit fit = scorer.score(RemotePathParser::parse(u"Bicep - Glue.mp3"_s), 269);
    EXPECT_FALSE(fit.fits);
    EXPECT_DOUBLE_EQ(fit.fit, 0.0);
}

} // namespace
