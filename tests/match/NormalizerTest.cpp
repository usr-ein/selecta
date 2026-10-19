// SPDX-License-Identifier: GPL-3.0-or-later
// Normaliser (note §4.2): field keys, artist sets, title decomposition into keys, album keys, mojibake, CamelCase.
#include "match/Normalizer.h"

#include "MatchTestSupport.h"

#include <QElapsedTimer>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::FieldKind;
using selecta::match::Normalizer;
using selecta::match::VersionClass;

namespace {

struct NormalizeCase
{
    const char *name = "";
    QString input;
    FieldKind kind = FieldKind::Plain;
    QString expected;
};

void PrintTo(const NormalizeCase &c, std::ostream *os)
{
    *os << c.name;
}

class NormalizeTest : public testing::TestWithParam<NormalizeCase>
{};

TEST_P(NormalizeTest, GivesTheKey)
{
    const NormalizeCase &c = GetParam();
    EXPECT_EQ(Normalizer::normalize(c.input, c.kind), c.expected);
}

TEST_P(NormalizeTest, IsIdempotent)
{
    const NormalizeCase &c = GetParam();
    const QString once = Normalizer::normalize(c.input, c.kind);
    EXPECT_EQ(Normalizer::normalize(once, c.kind), once);
}

INSTANTIATE_TEST_SUITE_P(
    Normalizer, NormalizeTest,
    testing::Values(
        // Diacritics and letters that don't decompose (step 3–4; T09, T67)
        NormalizeCase{"Roeyksopp", u"Röyksopp"_s, FieldKind::Artist, u"royksopp"_s},
        NormalizeCase{"Beyonce", u"Beyoncé"_s, FieldKind::Artist, u"beyonce"_s},
        NormalizeCase{"SigurRos", u"Sigur Rós"_s, FieldKind::Artist, u"sigur ros"_s},
        NormalizeCase{"Hoppipolla", u"Hoppípolla"_s, FieldKind::Title, u"hoppipolla"_s},
        NormalizeCase{"Mo", u"Mø"_s, FieldKind::Artist, u"mo"_s},
        NormalizeCase{"Aeon", u"Æon"_s, FieldKind::Title, u"aeon"_s},
        NormalizeCase{"Strasse", u"Straße"_s, FieldKind::Title, u"strasse"_s},
        NormalizeCase{"Lodz", u"Łódź"_s, FieldKind::Title, u"lodz"_s},
        NormalizeCase{"Thor", u"Þór"_s, FieldKind::Title, u"thor"_s},
        NormalizeCase{"Eth", u"Ðað"_s, FieldKind::Title, u"dad"_s},
        NormalizeCase{"DottedCapitalI", u"İstanbul"_s, FieldKind::Title, u"istanbul"_s},
        NormalizeCase{"DotlessI", u"Kırmızı"_s, FieldKind::Title, u"kirmizi"_s},
        NormalizeCase{"Ligature", u"ﬁnal"_s, FieldKind::Title, u"final"_s},
        NormalizeCase{"FullWidth", u"Ｆｕｌｌ"_s, FieldKind::Title, u"full"_s},
        NormalizeCase{"RomanNumeralSign", u"Ⅻ"_s, FieldKind::Title, u"xii"_s},
        NormalizeCase{"NfdInput", u"Café"_s, FieldKind::Title, u"cafe"_s},
        NormalizeCase{"Cyrillic", u"Москва"_s, FieldKind::Title, u"москва"_s},
        NormalizeCase{"Greek", u"Αθήνα"_s, FieldKind::Title, u"αθηνα"_s},
        NormalizeCase{"Kanji", u"東京"_s, FieldKind::Title, u"東京"_s},
        NormalizeCase{"Hangul", u"한국"_s, FieldKind::Title, u"한국"_s},
        // Symbols (step 5; T13, T28, T29, T64, T77)
        NormalizeCase{"Slash", u"AC/DC"_s, FieldKind::Artist, u"ac dc"_s},
        NormalizeCase{"Underscore", u"AC_DC"_s, FieldKind::Artist, u"ac dc"_s},
        NormalizeCase{"Ampersand", u"Above & Beyond"_s, FieldKind::Artist, u"above and beyond"_s},
        NormalizeCase{"Plus", u"Dan + Shay"_s, FieldKind::Artist, u"dan and shay"_s},
        NormalizeCase{"Dollar", u"Ke$ha"_s, FieldKind::Artist, u"kesha"_s},
        NormalizeCase{"DollarInside", u"A$AP Rocky"_s, FieldKind::Artist, u"asap rocky"_s},
        NormalizeCase{"Apostrophe", u"What's Going On"_s, FieldKind::Title, u"whats going on"_s},
        NormalizeCase{"CurlyApostrophe", u"What’s Going On"_s, FieldKind::Title, u"whats going on"_s},
        NormalizeCase{"AcronymDots", u"A.T.F.C."_s, FieldKind::Artist, u"atfc"_s},
        NormalizeCase{"AcronymDotsShort", u"M.K."_s, FieldKind::Artist, u"mk"_s},
        NormalizeCase{"AcronymMixedCase", u"t.A.T.u."_s, FieldKind::Artist, u"tatu"_s},
        NormalizeCase{"SingleInitial", u"J. Cole"_s, FieldKind::Artist, u"j cole"_s},
        NormalizeCase{"Abbreviation", u"Dr. Dre"_s, FieldKind::Artist, u"dr dre"_s},
        NormalizeCase{"Hyphen", u"Jay-Z"_s, FieldKind::Artist, u"jay z"_s},
        NormalizeCase{"Digits", u"Blink-182"_s, FieldKind::Artist, u"blink 182"_s},
        NormalizeCase{"EmDash", u"Hello—World"_s, FieldKind::Title, u"hello world"_s},
        NormalizeCase{"Spaces", u"  many   spaces\there "_s, FieldKind::Title, u"many spaces here"_s},
        NormalizeCase{"OnlyPunctuation", u"!?!"_s, FieldKind::Title, QString()},
        NormalizeCase{"Empty", QString(), FieldKind::Title, QString()},
        // Artist rules (step 7; T45)
        NormalizeCase{"LeadingThe", u"The Prodigy"_s, FieldKind::Artist, u"prodigy"_s},
        NormalizeCase{"CommaThe", u"Prodigy, The"_s, FieldKind::Artist, u"prodigy"_s},
        NormalizeCase{"BandCalledTheThe", u"The The"_s, FieldKind::Artist, u"the the"_s},
        NormalizeCase{"OnlyThe", u"The"_s, FieldKind::Artist, u"the"_s},
        NormalizeCase{"TitleKeepsThe", u"The Prodigy"_s, FieldKind::Title, u"the prodigy"_s},
        NormalizeCase{"CommaA", u"Tribe Called Quest, A"_s, FieldKind::Artist, u"a tribe called quest"_s},
        // CamelCase (§17.4; T54, T64)
        NormalizeCase{"CamelCase", u"OkumaDrift"_s, FieldKind::Artist, u"okuma drift"_s},
        NormalizeCase{"AcronymThenWord", u"DJKoze"_s, FieldKind::Artist, u"dj koze"_s},
        NormalizeCase{"TikTok", u"TiK ToK"_s, FieldKind::Title, u"tik tok"_s},
        NormalizeCase{"PluralAcronym", u"MCs"_s, FieldKind::Title, u"mcs"_s},
        NormalizeCase{"AllCaps", u"MGMT"_s, FieldKind::Artist, u"mgmt"_s},
        // Light canonicalisation (step 8; T63)
        NormalizeCase{"PtTwo", u"Title Pt. 2"_s, FieldKind::Title, u"title part 2"_s},
        NormalizeCase{"PartRoman", u"Title Part II"_s, FieldKind::Title, u"title part 2"_s},
        NormalizeCase{"VolumeRoman", u"Vol. III"_s, FieldKind::Album, u"volume 3"_s},
        NormalizeCase{"PartWord", u"Part Two"_s, FieldKind::Title, u"part 2"_s},
        NormalizeCase{"ChapterRoman", u"Chapter IV"_s, FieldKind::Title, u"chapter 4"_s},
        NormalizeCase{"RomanWithoutNumberingWord", u"Henry VIII"_s, FieldKind::Title, u"henry viii"_s},
        NormalizeCase{"MixIsNotANumeral", u"Part Mix"_s, FieldKind::Title, u"part mix"_s},
        NormalizeCase{"ArtistNotCanonicalised", u"PT Adamczyk"_s, FieldKind::Artist, u"pt adamczyk"_s},
        // Mojibake (P1; T47)
        NormalizeCase{"Mojibake", u"RÃ¶yksopp"_s, FieldKind::Artist, u"royksopp"_s},
        NormalizeCase{"DoubleMojibake", u"RÃƒÂ¶yksopp"_s, FieldKind::Artist, u"royksopp"_s}),
    [](const testing::TestParamInfo<NormalizeCase> &info) { return std::string(info.param.name); });

TEST(Normalizer, NfdAndNfcGiveTheSameKey) // T35: a macOS peer sends NFD
{
    const QString nfc = u"Beyoncé"_s;
    const QString nfd = nfc.normalized(QString::NormalizationForm_D);
    ASSERT_NE(nfc, nfd);
    EXPECT_EQ(Normalizer::normalize(nfd, FieldKind::Artist), Normalizer::normalize(nfc, FieldKind::Artist));
    EXPECT_EQ(Normalizer::trackKeys(nfd, u"Déjà Vu"_s).exactKey,
              Normalizer::trackKeys(nfc, u"Déjà Vu"_s.normalized(QString::NormalizationForm_D)).exactKey);
}

TEST(Normalizer, NormalisingTwiceChangesNothing) // property test
{
    selecta::match::test::Random random(0x5E1EC7AULL);
    for (int i = 0; i < 4000; ++i) {
        const QString text = selecta::match::test::randomText(random, 40);
        for (const FieldKind kind : {FieldKind::Plain, FieldKind::Title, FieldKind::Album, FieldKind::Artist}) {
            const QString once = Normalizer::normalize(text, kind);
            ASSERT_EQ(Normalizer::normalize(once, kind), once) << "input: " << text.toStdString();
            ASSERT_FALSE(once.startsWith(u' ') || once.endsWith(u' ') || once.contains(u"  "_s));
        }
    }
}

TEST(Normalizer, TokensAreTheKeyWords)
{
    EXPECT_EQ(Normalizer::tokens(u"Leave the World Behind"_s, FieldKind::Title),
              (QStringList{u"leave"_s, u"the"_s, u"world"_s, u"behind"_s}));
    EXPECT_TRUE(Normalizer::tokens(u"..."_s).isEmpty());
}

TEST(Normalizer, SquashRemovesSpaces)
{
    EXPECT_EQ(Normalizer::squash(u"dead mau5"_s), u"deadmau5"_s);
    EXPECT_EQ(Normalizer::squash(u"ac dc"_s), u"acdc"_s);
    EXPECT_EQ(Normalizer::squash(QString()), QString());
}

struct LeetCase
{
    const char *name = "";
    QString input;
    FieldKind kind = FieldKind::Plain;
    QString expected;
};

void PrintTo(const LeetCase &c, std::ostream *os)
{
    *os << c.name;
}

class LeetKeyTest : public testing::TestWithParam<LeetCase>
{};

TEST_P(LeetKeyTest, FoldsMixedWordsOnly) // §17.3: "never edit the digits"
{
    const LeetCase &c = GetParam();
    EXPECT_EQ(Normalizer::leetKey(c.input, c.kind), c.expected);
    EXPECT_EQ(Normalizer::fieldKey(c.input, c.kind).leet, c.expected);
}

INSTANTIATE_TEST_SUITE_P(Normalizer, LeetKeyTest,
                         testing::Values(LeetCase{"ExclamationInside", u"P!nk"_s, FieldKind::Artist, u"pink"_s},
                                         LeetCase{"PlainWord", u"Pink"_s, FieldKind::Artist, u"pink"_s},
                                         LeetCase{"DigitInWord", u"deadmau5"_s, FieldKind::Artist, u"deadmaus"_s},
                                         LeetCase{"LettersOnly", u"Deadmaus"_s, FieldKind::Artist, u"deadmaus"_s},
                                         LeetCase{"PureNumber", u"Blink-182"_s, FieldKind::Artist, u"blink182"_s},
                                         LeetCase{"Dollar", u"Ke$ha"_s, FieldKind::Artist, u"kesha"_s},
                                         LeetCase{"S3rl", u"S3RL"_s, FieldKind::Artist, u"serl"_s},
                                         LeetCase{"TrailingExclamation", u"Panic! at the Disco"_s, FieldKind::Artist,
                                                  u"panicatthedisco"_s},
                                         LeetCase{"LeadingThe", u"The Prodigy"_s, FieldKind::Artist, u"prodigy"_s},
                                         LeetCase{"AtSign", u"M@d"_s, FieldKind::Artist, u"mad"_s}),
                         [](const testing::TestParamInfo<LeetCase> &info) { return std::string(info.param.name); });

TEST(Normalizer, ArtistSetsSplitOnSeparatorsOnly) // note §4.1 step 7; T13, T14, T07
{
    const auto above = Normalizer::artistKeys(u"Above & Beyond"_s);
    EXPECT_EQ(above.field.key, u"above and beyond"_s);
    EXPECT_EQ(above.set, (QStringList{u"above"_s, u"beyond"_s}));
    EXPECT_EQ(Normalizer::artistKeys(u"Above and Beyond"_s).set, above.set);
    EXPECT_EQ(Normalizer::artistKeys(u"B & A"_s).set, Normalizer::artistKeys(u"A & B"_s).set);
    EXPECT_EQ(Normalizer::artistKeys(u"Earth, Wind & Fire"_s).set, (QStringList{u"earth"_s, u"fire"_s, u"wind"_s}));
    EXPECT_EQ(Normalizer::artistKeys(u"Jay-Z"_s).set, QStringList{u"jay z"_s});
    EXPECT_EQ(Normalizer::artistKeys(u"Lil Nas X"_s).set, QStringList{u"lil nas x"_s});
    EXPECT_EQ(Normalizer::artistKeys(u"Florence + The Machine"_s).set, (QStringList{u"florence"_s, u"machine"_s}));
    EXPECT_EQ(Normalizer::artistKeys(u"Skrillex x Diplo"_s).set, (QStringList{u"diplo"_s, u"skrillex"_s}));
    EXPECT_EQ(Normalizer::artistKeys(u"Daft Punk vs. Justice"_s).set, (QStringList{u"daft punk"_s, u"justice"_s}));
}

TEST(Normalizer, FeaturedArtistsNeverEnterThePrimaryKey) // T12
{
    for (const QString &text : {u"Artist feat. Guest"_s, u"Artist ft. Guest"_s, u"Artist featuring Guest"_s,
                                u"Artist (feat. Guest)"_s, u"Artist [ft. Guest]"_s, u"Artist Feat Guest"_s}) {
        const auto keys = Normalizer::artistKeys(text);
        EXPECT_EQ(keys.field.key, u"artist"_s) << text.toStdString();
        EXPECT_EQ(keys.set, QStringList{u"artist"_s}) << text.toStdString();
        EXPECT_EQ(keys.featured, QStringList{u"guest"_s}) << text.toStdString();
    }
    EXPECT_EQ(Normalizer::artistKeys(u"Artist ft. A & B"_s).featured, (QStringList{u"a"_s, u"b"_s}));
    // Words that merely start like a marker stay.
    EXPECT_EQ(Normalizer::artistKeys(u"Feather Witch"_s).field.key, u"feather witch"_s);
    EXPECT_EQ(Normalizer::artistKeys(u"Ftw Crew"_s).field.key, u"ftw crew"_s);
}

TEST(Normalizer, ArticleSuffixMovesToTheFront) // T45
{
    EXPECT_EQ(Normalizer::artistKeys(u"Prodigy, The"_s).field.key, u"prodigy"_s);
    EXPECT_EQ(Normalizer::artistKeys(u"The Prodigy"_s).field.key, u"prodigy"_s);
    EXPECT_EQ(Normalizer::artistKeys(u"Prodigy"_s).field.key, u"prodigy"_s);
    EXPECT_EQ(Normalizer::artistKeys(u"Prodigy, The"_s).set, QStringList{u"prodigy"_s});
}

TEST(Normalizer, TitleKeysSeparateVersionFeatAndExtras)
{
    const auto glue = Normalizer::titleKeys(u"Glue (Original Mix)"_s);
    EXPECT_EQ(glue.field.key, u"glue"_s);
    EXPECT_EQ(glue.version.versionClass, VersionClass::Original);

    const auto feat = Normalizer::titleKeys(u"Title (feat. Guest)"_s);
    EXPECT_EQ(feat.field.key, u"title"_s);
    EXPECT_EQ(feat.featured, QStringList{u"guest"_s});

    const auto intro = Normalizer::titleKeys(u"Title (Intro)"_s); // T31
    EXPECT_EQ(intro.field.key, u"title"_s);
    EXPECT_EQ(intro.extraKey, u"intro"_s);

    const auto segment = Normalizer::titleKeys(u"Song - Extended Mix"_s); // T44
    EXPECT_EQ(segment.field.key, u"song"_s);
    EXPECT_EQ(segment.version.versionClass, VersionClass::Extended);

    const auto remaster = Normalizer::titleKeys(u"Title (2011 Remaster)"_s); // T30
    EXPECT_EQ(remaster.field.key, u"title"_s);
    EXPECT_TRUE(remaster.version.remaster);
    EXPECT_EQ(remaster.version.versionClass, VersionClass::Unspecified);

    const auto junk = Normalizer::titleKeys(u"Title (Official Video) [HQ]"_s); // T26
    EXPECT_EQ(junk.field.key, u"title"_s);
    EXPECT_TRUE(junk.extraKey.isEmpty());

    const auto label = Normalizer::titleKeys(u"Title (Extended Mix) [Toolroom]"_s);
    EXPECT_EQ(label.labelHint, u"Toolroom"_s);
    EXPECT_TRUE(label.extraKey.isEmpty());

    EXPECT_EQ(Normalizer::titleKeys(u"Title (Part 2)"_s).field.key, u"title part 2"_s);
    EXPECT_EQ(Normalizer::titleKeys(u"(Untitled)"_s).field.key, u"untitled"_s);
}

TEST(Normalizer, TrackKeysFollowTheSchema) // note §4.2 step 9, §13
{
    const auto keys = Normalizer::trackKeys(u"Daft Punk"_s, u"Digital Love"_s, u"Discovery"_s);
    EXPECT_EQ(keys.normVersion, selecta::match::kNormVersion);
    EXPECT_EQ(keys.exactKey, u"daftpunk␟digitallove␟"_s);
    EXPECT_EQ(keys.looseKey, u"daftpunk␟digitallove"_s);
    EXPECT_EQ(keys.album.key, u"discovery"_s);

    const auto remix = Normalizer::trackKeys(u"Other Artist"_s, u"Glue (Bicep Remix)"_s);
    EXPECT_EQ(remix.exactKey, u"otherartist␟glue␟remix:bicep"_s);

    const auto pair = Normalizer::trackKeys(u"B & A"_s, u"Title"_s);
    EXPECT_EQ(pair.looseKey, u"a␟title"_s); // first member of the sorted set
}

TEST(Normalizer, AlbumKeysDropEditionText) // note §6 s_album
{
    EXPECT_EQ(Normalizer::albumKey(u"Discovery (Deluxe Edition)"_s).key, u"discovery"_s);
    EXPECT_EQ(Normalizer::albumKey(u"Isles [2021]"_s).key, u"isles"_s);
    EXPECT_EQ(Normalizer::albumKey(u"Album (Remastered 2011)"_s).key, u"album"_s);
    EXPECT_EQ(Normalizer::albumKey(u"Greatest Hits - Expanded Edition"_s).key, u"greatest hits"_s);
    EXPECT_EQ(Normalizer::albumKey(u"Album [FLAC]"_s).key, u"album"_s);
    EXPECT_EQ(Normalizer::albumKey(u"Album (EP)"_s).key, u"album"_s);
    EXPECT_EQ(Normalizer::albumKey(u"(Deluxe)"_s).key, u"deluxe"_s); // nothing but edition text: kept
    EXPECT_EQ(Normalizer::albumKey(u"Album (Live in Paris)"_s).key, u"album live in paris"_s);
}

TEST(Normalizer, RepairsMojibakeStrictly) // note §2.2, T47
{
    EXPECT_EQ(Normalizer::repairMojibake(u"RÃ¶yksopp"_s), u"Röyksopp"_s);
    EXPECT_EQ(Normalizer::repairMojibake(u"BeyoncÃ©"_s), u"Beyoncé"_s);
    EXPECT_EQ(Normalizer::repairMojibake(u"donâ€™t"_s), u"don’t"_s); // read as Windows-1252
    EXPECT_EQ(Normalizer::repairMojibake(u"RÃƒÂ¶yksopp"_s), u"Röyksopp"_s);
    // Untouched: correct text, a lone "Ã", and text with characters Latin-1 can't hold.
    EXPECT_EQ(Normalizer::repairMojibake(u"Röyksopp"_s), u"Röyksopp"_s);
    EXPECT_EQ(Normalizer::repairMojibake(u"Ãlbum"_s), u"Ãlbum"_s);
    EXPECT_EQ(Normalizer::repairMojibake(u"Ã¶ Москва"_s), u"Ã¶ Москва"_s);
    const QString repaired = Normalizer::repairMojibake(u"RÃ¶yksopp"_s);
    EXPECT_EQ(Normalizer::repairMojibake(repaired), repaired);
}

TEST(Normalizer, SplitsCamelCaseOnlyAtWordBoundaries)
{
    EXPECT_EQ(Normalizer::splitCamelCase(u"OkumaDrift"_s), u"Okuma Drift"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"DJKoze"_s), u"DJ Koze"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"KRSOne"_s), u"KRS One"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"TiK ToK"_s), u"TiK ToK"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"MCs DJs"_s), u"MCs DJs"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"MGMT"_s), u"MGMT"_s);
    EXPECT_EQ(Normalizer::splitCamelCase(u"deadmau5"_s), u"deadmau5"_s);
}

TEST(Normalizer, BoundsHugeInput) // untrusted input: the work per call is bounded
{
    QString huge;
    for (int i = 0; i < 20000; ++i) {
        huge += u"Röyksopp & "_s;
    }
    QElapsedTimer timer;
    timer.start();
    const QString key = Normalizer::normalize(huge, FieldKind::Artist);
    EXPECT_LE(key.size(), Normalizer::kMaxFieldLength * 2);
    EXPECT_LT(timer.elapsed(), 1000);
    const auto keys = Normalizer::trackKeys(huge, huge, huge);
    EXPECT_FALSE(keys.exactKey.isEmpty());
}

TEST(Normalizer, SurvivesBrokenUtf16)
{
    const QString lone = QString(QChar(0xD83D)) + u"abc"_s + QChar(0xDC00);
    EXPECT_EQ(Normalizer::normalize(lone), u"abc"_s);
    EXPECT_EQ(Normalizer::normalize(Normalizer::normalize(lone)), Normalizer::normalize(lone));
}

} // namespace
