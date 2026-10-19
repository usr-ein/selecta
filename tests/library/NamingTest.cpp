// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/Naming.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
namespace naming = selecta::library::naming;
using naming::Cleaner;
using naming::Fields;

namespace {

Fields single(const QString &artist, const QString &title)
{
    Fields fields;
    fields.artist = artist;
    fields.title = title;
    return fields;
}

std::string str(const QString &text)
{
    return text.toStdString();
}

} // namespace

TEST(Naming, PrdExample)
{
    // PRD DL-13: `Halden & Rue - Tidewater (Original Mix)` → `Halden_and_Rue_-_Tidewater_Original_Mix.mp3`.
    const QString name = naming::componentName(
        u"{artist} - {title}", single(u"Halden & Rue"_s, u"Tidewater (Original Mix)"_s), Cleaner{}, u"mp3"_s);
    EXPECT_EQ(str(name), "Halden_and_Rue_-_Tidewater_Original_Mix.mp3");
}

TEST(Naming, CleanerKeepsUnicodeLettersAndDropsQuotes)
{
    EXPECT_EQ(str(naming::clean(u"Sigur Rós – Hoppípolla"_s, u'_')), "Sigur_Rós_Hoppípolla");
    EXPECT_EQ(str(naming::clean(u"Don't Stop “Now”"_s, u'_')), "Dont_Stop_Now");
    EXPECT_EQ(str(naming::clean(u"100% $pecial: A/B?*|"_s, u'_')), "100_pecial_A_B");
    EXPECT_EQ(str(naming::clean(u"...hidden name..."_s, u'_')), "hidden_name");
    EXPECT_EQ(str(naming::clean(u"Мой трек (Remix)"_s, u'_')), "Мой_трек_Remix");
    EXPECT_EQ(str(naming::clean(u"a _ b"_s, u'_')), "a_b"); // a separator typed in the name collapses too
}

TEST(Naming, CleanerNormalisesToNfc)
{
    const QString decomposed = u"Cafe\u0301"_s; // "e" + combining acute, as macOS peers send it
    EXPECT_EQ(naming::clean(decomposed, u'_'), u"Caf\u00E9"_s);
}

TEST(Naming, OtherSeparators)
{
    EXPECT_EQ(str(naming::clean(u"Halden & Rue - Tidewater (Original Mix)"_s, u' ')),
              "Halden and Rue - Tidewater Original Mix");
    EXPECT_EQ(str(naming::clean(u"A & B"_s, u'.')), "A.and.B");
}

TEST(Naming, CleanerCanBeTurnedOffButNamesStaySafe)
{
    const Cleaner off{.enabled = false, .separator = u'_'};
    EXPECT_EQ(
        str(naming::componentName(u"{artist} - {title}", single(u"AC/DC"_s, u"Back: In Black?"_s), off, u"mp3"_s)),
        "AC-DC - Back- In Black_.mp3");
}

TEST(Naming, OptionalSectionsAndPadding)
{
    Fields album;
    album.albumArtist = u"Okuma Drift"_s;
    album.album = u"Nightfall"_s;
    const Cleaner off{.enabled = false, .separator = u'_'};
    EXPECT_EQ(str(naming::componentName(u"{albumartist} - {album}[ ({year})]", album, off)), "Okuma Drift - Nightfall");
    album.year = u"2024"_s;
    EXPECT_EQ(str(naming::componentName(u"{albumartist} - {album}[ ({year})]", album, off)),
              "Okuma Drift - Nightfall (2024)");

    Fields track = single(u"Okuma Drift"_s, u"Neon Rain"_s);
    track.track = u"3"_s;
    EXPECT_EQ(str(naming::expand(u"{track} - {title}", track)), "03 - Neon Rain");
    EXPECT_EQ(str(naming::expand(u"{track:3} - {title}", track)), "003 - Neon Rain");
    EXPECT_EQ(str(naming::expand(u"[{disc}-]{track} {title}", track)), "03 Neon Rain");
    track.disc = u"2"_s;
    EXPECT_EQ(str(naming::expand(u"[{disc}-]{track} {title}", track)), "2-03 Neon Rain");
    EXPECT_EQ(str(naming::expand(u"{unknown}{title}", track)), "Neon Rain");
    EXPECT_EQ(str(naming::expand(u"[{key}[ {bpm}]] {title}", track)), " Neon Rain");              // nested, both empty
    EXPECT_EQ(str(naming::expand(u"broken [section {title}", track)), "broken [section {title}"); // literal
}

TEST(Naming, MissingValuesNeverLeaveAnEmptyName)
{
    EXPECT_EQ(str(naming::componentName(u"{artist} - {title}", Fields{}, Cleaner{}, u"mp3"_s)),
              "Unknown_Artist_-_Untitled.mp3");
    EXPECT_EQ(str(naming::componentName(u"{label}", Fields{}, Cleaner{})), "Untitled");
    EXPECT_EQ(str(naming::safeComponent(QString())), "Untitled");
    EXPECT_EQ(str(naming::safeComponent(u".."_s)), "Untitled");
}

TEST(Naming, DangerousCharactersAreRemoved)
{
    // U+202E (right-to-left override) would make "x<U+202E>gpj.mp3" display as "x3pm.jpg".
    EXPECT_EQ(str(naming::safeComponent(u"x\u202Egpj"_s)), "xgpj");
    EXPECT_EQ(str(naming::safeComponent(u"tab\there\nnewline\u0000"_s)), "tabherenewline");
    EXPECT_EQ(str(naming::safeComponent(u"\uFEFFbom"_s)), "bom");
}

TEST(Naming, EdgesAndReservedNames)
{
    EXPECT_EQ(str(naming::safeComponent(u".hidden"_s)), "_hidden");
    EXPECT_EQ(str(naming::safeComponent(u"-rf"_s)), "_rf");
    EXPECT_EQ(str(naming::safeComponent(u"trailing. . "_s)), "trailing");
    EXPECT_EQ(str(naming::safeComponent(u"CON"_s)), "CON_");
    EXPECT_EQ(str(naming::safeComponent(u"nul.txt"_s)), "nul_.txt");
    EXPECT_EQ(str(naming::safeComponent(u"../../etc/passwd"_s)), "_.-..-etc-passwd");
}

TEST(Naming, LongNamesAreCutBetweenCharacters)
{
    // 150 two-byte letters = 300 bytes: cut to at most 200 bytes, never inside a character.
    const QString longTitle(150, QChar(0x00E9));
    const QString name = naming::componentName(u"{title}", single(u"A"_s, longTitle), Cleaner{}, u"mp3"_s);
    EXPECT_LE(name.toUtf8().size(), naming::kMaxComponentBytes);
    EXPECT_TRUE(name.endsWith(u".mp3"_s));
    EXPECT_FALSE(name.contains(QChar::ReplacementCharacter));

    // Emoji with modifiers are single graphemes and must not be split.
    QString emoji;
    for (int i = 0; i < 60; ++i) {
        emoji += u"\U0001F44D\U0001F3FD"_s; // thumbs up, medium skin tone: 8 bytes
    }
    const QString cut = naming::safeComponent(emoji, 20);
    EXPECT_LE(cut.toUtf8().size(), 20);
    EXPECT_EQ(cut.size() % 4, 0); // whole graphemes of 4 UTF-16 units each
}
