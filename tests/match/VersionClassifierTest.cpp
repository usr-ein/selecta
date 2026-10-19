// SPDX-License-Identifier: GPL-3.0-or-later
// Version classification and compatibility (note §5), title decomposition (note §4.1 steps 7–8).
#include "match/VersionClassifier.h"

#include "MatchTestSupport.h"
#include "match/Normalizer.h"

#include <gtest/gtest.h>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using selecta::match::VersionClass;
using selecta::match::VersionClassifier;
using selecta::match::VersionCompatibility;
using selecta::match::VersionInfo;
using selecta::match::VersionModifier;
using selecta::match::VersionModifiers;

namespace {

struct ClassifyCase
{
    QString phrase;
    std::optional<VersionClass> expected; // nullopt: not version text
    QStringList by;
    VersionModifiers modifiers;
    bool remaster = false;
    bool mixed = false;
};

ClassifyCase C(QString phrase, std::optional<VersionClass> expected, QStringList by = {},
               VersionModifiers modifiers = {}, bool remaster = false, bool mixed = false)
{
    return {std::move(phrase), expected, std::move(by), modifiers, remaster, mixed};
}

void PrintTo(const ClassifyCase &c, std::ostream *os)
{
    *os << c.phrase.toStdString();
}

class ClassifyTest : public testing::TestWithParam<ClassifyCase>
{};

TEST_P(ClassifyTest, ClassifiesThePhrase)
{
    const ClassifyCase &c = GetParam();
    const std::optional<VersionInfo> version = VersionClassifier::classify(c.phrase);
    if (!c.expected) {
        EXPECT_FALSE(version.has_value());
        return;
    }
    if (!version) {
        ADD_FAILURE() << "not classified";
        return;
    }
    EXPECT_EQ(version->versionClass, *c.expected);
    EXPECT_EQ(version->by, c.by);
    EXPECT_EQ(version->modifiers, c.modifiers);
    EXPECT_EQ(version->remaster, c.remaster);
    EXPECT_EQ(version->mixed, c.mixed);
    EXPECT_EQ(version->raw, c.phrase.trimmed());
}

using M = VersionModifier;

INSTANTIATE_TEST_SUITE_P(
    Original, ClassifyTest,
    testing::Values(C(u"Original Mix"_s, VersionClass::Original), C(u"Original"_s, VersionClass::Original),
                    C(u"Original Version"_s, VersionClass::Original), C(u"Main Mix"_s, VersionClass::Original),
                    C(u"Album Version"_s, VersionClass::Original)),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(
    Extended, ClassifyTest,
    testing::Values(C(u"Extended Mix"_s, VersionClass::Extended), C(u"Extended Version"_s, VersionClass::Extended),
                    C(u"Extended Edit"_s, VersionClass::Extended), C(u"Extended"_s, VersionClass::Extended),
                    C(u"Club Mix"_s, VersionClass::Extended), C(u"12\" Mix"_s, VersionClass::Extended),
                    C(u"12 Inch Version"_s, VersionClass::Extended), C(u"Long Version"_s, VersionClass::Extended),
                    C(u"Full Length"_s, VersionClass::Extended), C(u"Extended Club Mix"_s, VersionClass::Extended)),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(
    Radio, ClassifyTest,
    testing::Values(C(u"Radio Edit"_s, VersionClass::Radio), C(u"Radio Mix"_s, VersionClass::Radio),
                    C(u"Radio Version"_s, VersionClass::Radio), C(u"Edit"_s, VersionClass::Radio),
                    C(u"Short Edit"_s, VersionClass::Radio), C(u"Single Version"_s, VersionClass::Radio),
                    C(u"7\" Version"_s, VersionClass::Radio), C(u"Single Edit"_s, VersionClass::Radio)),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(
    Remix, ClassifyTest,
    testing::Values(
        C(u"Bicep Remix"_s, VersionClass::Remix, {u"bicep"_s}), C(u"Bicep Mix"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Rework"_s, VersionClass::Remix, {u"bicep"_s}), C(u"Bicep Refix"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Flip"_s, VersionClass::Remix, {u"bicep"_s}), C(u"Bicep Bootleg"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Reconstruction"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Rerub"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Re-Work"_s, VersionClass::Remix, {u"bicep"_s}), C(u"Bicep RMX"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Version"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Bicep Edit"_s, VersionClass::Remix, {u"bicep"_s}, M::Edit),
        C(u"Bicep Re-Edit"_s, VersionClass::Remix, {u"bicep"_s}, M::Edit),
        C(u"Bicep Dub"_s, VersionClass::Remix, {u"bicep"_s}, M::Dub),
        C(u"Kerri Chandler's Vocal Dub"_s, VersionClass::Remix, {u"kerri chandler"_s},
          VersionModifiers(M::Vocal) | M::Dub),
        C(u"Kerri Chandler Extended Remix"_s, VersionClass::Remix, {u"kerri chandler"_s}, M::Extended),
        C(u"Radio Slave Remix"_s, VersionClass::Remix, {u"radio slave"_s}),
        C(u"Dub Phizix Remix"_s, VersionClass::Remix, {u"dub phizix"_s}),
        C(u"Bicep & Hammer Remix"_s, VersionClass::Remix, {u"bicep"_s, u"hammer"_s}),
        C(u"The Prodigy Remix"_s, VersionClass::Remix, {u"prodigy"_s}), C(u"Remix"_s, VersionClass::Remix),
        C(u"Extended Remix"_s, VersionClass::Remix, {}, M::Extended),
        C(u"Remixed by Bicep"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Remix by Bicep"_s, VersionClass::Remix, {u"bicep"_s}),
        C(u"Edit by Bicep"_s, VersionClass::Remix, {u"bicep"_s}, M::Edit),
        C(u"1999 Remix"_s, VersionClass::Remix, {u"1999"_s})),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(
    DjTools, ClassifyTest,
    testing::Values(C(u"VIP"_s, VersionClass::Vip), C(u"VIP Mix"_s, VersionClass::Vip),
                    C(u"Skrillex VIP"_s, VersionClass::Vip, {u"skrillex"_s}), C(u"Dub"_s, VersionClass::Dub),
                    C(u"Dub Mix"_s, VersionClass::Dub), C(u"Vocal Dub"_s, VersionClass::Dub, {}, M::Vocal),
                    C(u"Instrumental"_s, VersionClass::Instrumental),
                    C(u"Instrumental Mix"_s, VersionClass::Instrumental), C(u"Vocal Mix"_s, VersionClass::Vocal),
                    C(u"Acapella"_s, VersionClass::Acapella), C(u"A Cappella"_s, VersionClass::Acapella),
                    C(u"Acappella"_s, VersionClass::Acapella),
                    C(u"Extended Vocal Mix"_s, VersionClass::Extended, {}, M::Vocal),
                    C(u"Extended Instrumental"_s, VersionClass::Instrumental, {}, M::Extended),
                    C(u"VIP Dub"_s, VersionClass::Vip, {}, M::Dub)),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(
    LiveAcousticDemo, ClassifyTest,
    testing::Values(C(u"Live"_s, VersionClass::Live), C(u"Live Version"_s, VersionClass::Live),
                    C(u"Live at Brixton"_s, VersionClass::Live, {u"brixton"_s}),
                    C(u"Live @ Fabric"_s, VersionClass::Live, {u"fabric"_s}), C(u"Acoustic"_s, VersionClass::Acoustic),
                    C(u"Unplugged"_s, VersionClass::Acoustic), C(u"Acoustic Version"_s, VersionClass::Acoustic),
                    C(u"Demo"_s, VersionClass::Demo)),
    [](const auto &info) { return selecta::match::test::testName(info.param.phrase.toStdString()); });

INSTANTIATE_TEST_SUITE_P(Flags, ClassifyTest,
                         testing::Values(C(u"2011 Remaster"_s, VersionClass::Unspecified, {}, {}, true),
                                         C(u"Remastered 2009"_s, VersionClass::Unspecified, {}, {}, true),
                                         C(u"Remastered"_s, VersionClass::Unspecified, {}, {}, true),
                                         C(u"Digitally Remastered"_s, VersionClass::Unspecified, {}, {}, true),
                                         C(u"2015 Remastered Version"_s, VersionClass::Unspecified, {}, {}, true),
                                         C(u"Remastered Extended Mix"_s, VersionClass::Extended, {}, {}, true),
                                         C(u"Mixed"_s, VersionClass::Unspecified, {}, {}, false, true),
                                         C(u"Mixed by Bicep"_s, VersionClass::Unspecified, {}, {}, false, true),
                                         C(u"DJ Mix"_s, VersionClass::Unspecified, {}, {}, false, true)),
                         [](const auto &info) {
                             return selecta::match::test::testName(info.param.phrase.toStdString());
                         });

INSTANTIATE_TEST_SUITE_P(NotAVersion, ClassifyTest,
                         testing::Values(C(u"Intro"_s, std::nullopt), C(u"Part 2"_s, std::nullopt),
                                         C(u"feat. Guest"_s, std::nullopt), C(u"Mix"_s, std::nullopt),
                                         C(u"Version"_s, std::nullopt), C(u"Clean Version"_s, std::nullopt),
                                         C(u"Spanish Version"_s, std::nullopt), C(u"Interlude"_s, std::nullopt),
                                         C(u"Something Else"_s, std::nullopt), C(u"Single"_s, std::nullopt),
                                         C(QString(), std::nullopt), C(u"   "_s, std::nullopt)),
                         [](const auto &info) {
                             return selecta::match::test::testName(info.param.phrase.toStdString());
                         });

VersionInfo version(const QString &phrase)
{
    if (phrase.isEmpty()) {
        return {};
    }
    const auto classified = VersionClassifier::classify(phrase);
    EXPECT_TRUE(classified.has_value()) << phrase.toStdString();
    return classified.value_or(VersionInfo{});
}

TEST(VersionClassifier, KeysAreCanonical)
{
    EXPECT_EQ(VersionInfo{}.key(), QString());
    EXPECT_EQ(version(u"Original Mix"_s).key(), u"original"_s);
    EXPECT_EQ(version(u"Bicep Remix"_s).key(), u"remix:bicep"_s);
    EXPECT_EQ(version(u"Bicep Dub"_s).key(), u"remix:bicep+dub"_s);
    EXPECT_EQ(version(u"Extended Vocal Mix"_s).key(), u"extended+vocal"_s);
    EXPECT_EQ(version(u"dead mau5 Remix"_s).key(), version(u"deadmau5 Remix"_s).key());
    EXPECT_EQ(version(u"Hammer & Bicep Remix"_s).key(), version(u"Bicep and Hammer Remix"_s).key());
    EXPECT_EQ(version(u"2011 Remaster"_s).key(), QString()); // remaster is not a version
}

struct CompareCase
{
    const char *name = "";
    QString a;
    QString b;
    VersionCompatibility expected = VersionCompatibility::Same;
};

void PrintTo(const CompareCase &c, std::ostream *os)
{
    *os << c.name;
}

class CompareTest : public testing::TestWithParam<CompareCase>
{};

TEST_P(CompareTest, FollowsTheCompatibilityTable) // note §5
{
    const CompareCase &c = GetParam();
    EXPECT_EQ(VersionClassifier::compare(version(c.a), version(c.b)), c.expected);
    EXPECT_EQ(VersionClassifier::compare(version(c.b), version(c.a)), c.expected) << "symmetric";
}

INSTANTIATE_TEST_SUITE_P(
    VersionClassifier, CompareTest,
    testing::Values(
        CompareCase{"BothUnspecified", QString(), QString(), VersionCompatibility::Same},
        CompareCase{"UnspecifiedOriginal_T02", QString(), u"Original Mix"_s, VersionCompatibility::ProbablySame},
        CompareCase{"UnspecifiedExtended_T03", QString(), u"Extended Mix"_s, VersionCompatibility::Unsure},
        CompareCase{"UnspecifiedRemix", QString(), u"Bicep Remix"_s, VersionCompatibility::Unsure},
        CompareCase{"RadioExtended_T04", u"Radio Edit"_s, u"Extended Mix"_s, VersionCompatibility::OtherVersion},
        CompareCase{"RemixerConflict_T05", u"Y Remix"_s, u"Z Remix"_s, VersionCompatibility::OtherVersion},
        CompareCase{"SameRemix", u"Bicep Remix"_s, u"BICEP Remix"_s, VersionCompatibility::Same},
        CompareCase{"UnnamedRemix", u"Remix"_s, u"Bicep Remix"_s, VersionCompatibility::Unsure},
        CompareCase{"SelfRemixVsOriginal_T46", u"Artist Remix"_s, u"Original Mix"_s,
                    VersionCompatibility::OtherVersion},
        CompareCase{"RemasterOnly_T30", QString(), u"2011 Remaster"_s, VersionCompatibility::Same},
        CompareCase{"OriginalVsRemasteredOriginal", u"Original Mix"_s, u"Original Mix"_s, VersionCompatibility::Same},
        CompareCase{"DubVsRemix", u"Bicep Dub"_s, u"Bicep Remix"_s, VersionCompatibility::OtherVersion},
        CompareCase{"EditVsRemix", u"Bicep Edit"_s, u"Bicep Remix"_s, VersionCompatibility::OtherVersion},
        CompareCase{"OriginalVsRadio_T40", u"Original Mix"_s, u"Radio Edit"_s, VersionCompatibility::OtherVersion},
        CompareCase{"ExtendedSpellings", u"Extended Mix"_s, u"Club Mix"_s, VersionCompatibility::Same},
        CompareCase{"LiveVenues", u"Live at Brixton"_s, u"Live at Wembley"_s, VersionCompatibility::OtherVersion},
        CompareCase{"LiveUnknownVenue", u"Live"_s, u"Live at Wembley"_s, VersionCompatibility::Unsure}),
    [](const testing::TestParamInfo<CompareCase> &info) { return std::string(info.param.name); });

struct DecomposeCase
{
    const char *name = "";
    QString title;
    QString core;
    VersionClass versionClass = VersionClass::Unspecified;
    QStringList featured;
    QStringList extras;
    QString labelHint;
    bool truncated = false;
};

DecomposeCase D(const char *name, QString title, QString core, VersionClass versionClass = VersionClass::Unspecified,
                QStringList featured = {}, QStringList extras = {}, QString labelHint = {}, bool truncated = false)
{
    return {name,
            std::move(title),
            std::move(core),
            versionClass,
            std::move(featured),
            std::move(extras),
            std::move(labelHint),
            truncated};
}

void PrintTo(const DecomposeCase &c, std::ostream *os)
{
    *os << c.name;
}

class DecomposeTest : public testing::TestWithParam<DecomposeCase>
{};

TEST_P(DecomposeTest, SplitsTheTitle)
{
    const DecomposeCase &c = GetParam();
    const auto parts = VersionClassifier::decompose(c.title);
    EXPECT_EQ(parts.core, c.core);
    EXPECT_EQ(parts.version.versionClass, c.versionClass);
    EXPECT_EQ(parts.featured, c.featured);
    EXPECT_EQ(parts.extras, c.extras);
    EXPECT_EQ(parts.labelHint, c.labelHint);
    EXPECT_EQ(parts.truncated, c.truncated);
}

INSTANTIATE_TEST_SUITE_P(
    VersionClassifier, DecomposeTest,
    testing::Values(
        D("Plain", u"Glue"_s, u"Glue"_s), D("OriginalMix", u"Glue (Original Mix)"_s, u"Glue"_s, VersionClass::Original),
        D("SquareBrackets", u"Glue [Extended Mix]"_s, u"Glue"_s, VersionClass::Extended),
        D("TrailingSegment_T44", u"Song - Extended Mix"_s, u"Song"_s, VersionClass::Extended),
        D("FeatThenVersion", u"Title (feat. X) (Extended Mix)"_s, u"Title"_s, VersionClass::Extended, {u"X"_s}),
        D("VersionThenFeat", u"Title [Extended Mix] (feat. X)"_s, u"Title"_s, VersionClass::Extended, {u"X"_s}),
        D("TwoVersionGroups", u"Title (Bicep Remix) (Radio Edit)"_s, u"Title"_s, VersionClass::Remix),
        D("NestedBrackets", u"Title (Bicep (UK) Remix)"_s, u"Title"_s, VersionClass::Remix),
        D("SubtitleIsNotAVersion", u"Title - Subtitle"_s, u"Title - Subtitle"_s),
        D("Live", u"Title (Live at Brixton)"_s, u"Title"_s, VersionClass::Live),
        D("CleanIsExtra", u"Song (Clean)"_s, u"Song"_s, VersionClass::Unspecified, {}, {u"Clean"_s}),
        D("IntroIsExtra_T31", u"Title (Intro)"_s, u"Title"_s, VersionClass::Unspecified, {}, {u"Intro"_s}),
        D("BonusTrackIsJunk", u"Song (Bonus Track)"_s, u"Song"_s),
        D("ProducerCreditIsJunk", u"Song (Prod. by X)"_s, u"Song"_s),
        D("QualityTagIsJunk", u"Song [FLAC]"_s, u"Song"_s), D("YearIsJunk", u"Song (1999)"_s, u"Song"_s),
        D("WebJunk_T26", u"Title (Official Video) [HQ]"_s, u"Title"_s),
        D("UnbracketedFeat", u"Song ft. Guest"_s, u"Song"_s, VersionClass::Unspecified, {u"Guest"_s}),
        D("PartStaysInCore", u"Title (Part 2) (Extended Mix)"_s, u"Title Part 2"_s, VersionClass::Extended),
        D("LabelHint", u"Title (Original Mix) [Toolroom]"_s, u"Title"_s, VersionClass::Original, {}, {}, u"Toolroom"_s),
        D("LabelWithoutVersionIsExtra", u"Title [Toolroom]"_s, u"Title"_s, VersionClass::Unspecified, {},
          {u"Toolroom"_s}),
        D("OnlyOneGroup", u"(Untitled)"_s, u"(Untitled)"_s),
        D("Truncated", u"Title (Extended Mi"_s, u"Title"_s, VersionClass::Unspecified, {}, {u"Extended Mi"_s}, {},
          true),
        D("RemasterFlag", u"Title (2011 Remaster)"_s, u"Title"_s), D("Empty", QString(), QString())),
    [](const testing::TestParamInfo<DecomposeCase> &info) { return std::string(info.param.name); });

TEST(VersionClassifier, MergedGroupsKeepTheRemixerAndTheModifier)
{
    const auto parts = VersionClassifier::decompose(u"Title (Bicep Remix) (Radio Edit)"_s);
    EXPECT_EQ(parts.version.by, QStringList{u"bicep"_s});
    EXPECT_TRUE(parts.version.modifiers.testFlag(VersionModifier::Radio));
    EXPECT_EQ(parts.version.raw, u"Bicep Remix / Radio Edit"_s);
}

TEST(VersionClassifier, SurvivesHostileInput)
{
    QString deep;
    for (int i = 0; i < 2000; ++i) {
        deep += u"(("_s;
    }
    deep += u"Title"_s;
    for (int i = 0; i < 1000; ++i) {
        deep += u")]"_s;
    }
    const auto parts = VersionClassifier::decompose(deep);
    EXPECT_LE(parts.core.size(), selecta::match::Normalizer::kMaxFieldLength);
    EXPECT_FALSE(VersionClassifier::classify(deep).has_value());
}

} // namespace
