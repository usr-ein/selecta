// SPDX-License-Identifier: GPL-3.0-or-later
// Names, aliases and words of the share index (detail/Text): what is shared by name, how names look to peers, how
// words are split and folded for search.
#include "share/detail/Text.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::share::detail::FileKind;
namespace detail = selecta::share::detail;

namespace {

// --- File kinds (decision D-S1) --------------------------------------------------------------------------------------

TEST(ShareText, SharesAudioCoversAndTextCompanionsOnly)
{
    for (const QString &name : {u"a.mp3"_s, u"a.flac"_s, u"a.wav"_s, u"a.aif"_s, u"a.aiff"_s, u"a.m4a"_s, u"a.alac"_s,
                                u"a.aac"_s, u"a.ogg"_s, u"a.opus"_s, u"a.wma"_s, u"a.wv"_s, u"a.ape"_s}) {
        EXPECT_EQ(detail::fileKind(name), FileKind::Audio) << name.toStdString();
    }
    for (const QString &name : {u"cover.jpg"_s, u"cover.jpeg"_s, u"folder.png"_s}) {
        EXPECT_EQ(detail::fileKind(name), FileKind::Image) << name.toStdString();
    }
    for (const QString &name : {u"a.cue"_s, u"a.log"_s, u"a.nfo"_s, u"a.txt"_s, u"booklet.pdf"_s}) {
        EXPECT_EQ(detail::fileKind(name), FileKind::Companion) << name.toStdString();
    }
    for (const QString &name : {u"a.exe"_s, u"a.zip"_s, u"a.gif"_s, u"a.m3u"_s, u"a.sfv"_s, u"a.mp3.part"_s,
                                u"README"_s, u"a."_s, u".mp3"_s, u""_s}) {
        EXPECT_EQ(detail::fileKind(name), FileKind::Other) << name.toStdString();
    }
}

TEST(ShareText, ExtensionsAreCaseInsensitiveAndLowerCased)
{
    EXPECT_EQ(detail::fileKind(u"Track.MP3"), FileKind::Audio);
    EXPECT_EQ(detail::fileKind(u"COVER.JPG"), FileKind::Image);
    EXPECT_EQ(detail::lowerExtension(u"Track.FLAC"), u"flac"_s);
    EXPECT_EQ(detail::lowerExtension(u"a.b.Mp3"), u"mp3"_s);
    EXPECT_TRUE(detail::lowerExtension(u"noextension").isEmpty());
    EXPECT_TRUE(detail::lowerExtension(u"trailing.").isEmpty());
    EXPECT_TRUE(detail::lowerExtension(u".hidden").isEmpty());
}

// --- Skip rules by name ----------------------------------------------------------------------------------------------

TEST(ShareText, HiddenTemporaryAndJunkNames)
{
    EXPECT_TRUE(detail::isHiddenName(u".DS_Store"));
    EXPECT_TRUE(detail::isHiddenName(u"._01 Track.mp3")); // AppleDouble
    EXPECT_FALSE(detail::isHiddenName(u"Track.mp3"));

    for (const QString &name : {u"a.mp3.selecta-part"_s, u"a.mp3.part"_s, u"a.mp3.PART"_s, u"a.partial"_s, u"a.tmp"_s,
                                u"a.temp"_s, u"a.crdownload"_s, u"a.download"_s, u"a.!qB"_s, u"~$a.mp3"_s,
                                u"a.mp3~"_s}) {
        EXPECT_TRUE(detail::isTemporaryName(name)) << name.toStdString();
    }
    EXPECT_FALSE(detail::isTemporaryName(u"Departure.mp3"));

    for (const QString &name : {u"$RECYCLE.BIN"_s, u"$Recycle.Bin"_s, u"System Volume Information"_s, u"@eaDir"_s,
                                u"#recycle"_s, u"#snapshot"_s, u"Network Trash Folder"_s, u"Temporary Items"_s}) {
        EXPECT_TRUE(detail::isJunkFolderName(name)) << name.toStdString();
    }
    EXPECT_FALSE(detail::isJunkFolderName(u"Recycled Sounds"));
}

// --- Names as peers see them -----------------------------------------------------------------------------------------

TEST(ShareText, LocalNamesBecomeNfcWithTheSeparatorReplaced)
{
    const QByteArray nfd = u"Cafe\u0301.mp3"_s.toUtf8(); // e + combining acute
    const detail::LocalName fromNfd = detail::localName(nfd);
    EXPECT_TRUE(fromNfd.problem.isEmpty());
    EXPECT_EQ(fromNfd.virtualName, u"Caf\u00e9.mp3"_s);
    EXPECT_EQ(fromNfd.virtualName, fromNfd.virtualName.normalized(QString::NormalizationForm_C));

    const detail::LocalName slash = detail::localName("AC\\DC - Back.mp3");
    EXPECT_EQ(slash.virtualName, u"AC＼DC - Back.mp3"_s);
    EXPECT_FALSE(slash.virtualName.contains(u'\\'));
    EXPECT_EQ(slash.key, u"AC\\DC - Back.mp3"_s); // the local spelling keeps it
}

TEST(ShareText, NamesThatCantBeSharedSayWhy)
{
    EXPECT_FALSE(detail::localName("bad \xff\xfe name.mp3").problem.isEmpty());
    EXPECT_FALSE(detail::localName("line\nbreak.mp3").problem.isEmpty());
    EXPECT_FALSE(detail::localName("tab\there.mp3").problem.isEmpty());
    EXPECT_FALSE(detail::localName("delete\x7f.mp3").problem.isEmpty());
    EXPECT_FALSE(detail::localName("c1 \xc2\x85 control.mp3").problem.isEmpty()); // U+0085
    const detail::LocalName bad = detail::localName("bad \xff.mp3");
    EXPECT_TRUE(bad.virtualName.isEmpty());
    EXPECT_TRUE(bad.key.isEmpty());
    EXPECT_TRUE(detail::localName("Ünïcödé 日本 – ok.flac").problem.isEmpty());
}

// --- Aliases ---------------------------------------------------------------------------------------------------------

TEST(ShareText, AliasesAreCleaned)
{
    EXPECT_EQ(detail::cleanAlias(u"Music"), u"Music"_s);
    EXPECT_EQ(detail::cleanAlias(u"  Music  "), u"Music"_s);
    EXPECT_EQ(detail::cleanAlias(u"A\\B/C"), u"A_B_C"_s);
    EXPECT_EQ(detail::cleanAlias(u"Tab\tName"), u"TabName"_s);
    EXPECT_EQ(detail::cleanAlias(u""), u"Shared"_s);
    EXPECT_EQ(detail::cleanAlias(u"   "), u"Shared"_s);
    EXPECT_EQ(detail::cleanAlias(u"."), u"Shared"_s);
    EXPECT_EQ(detail::cleanAlias(u".."), u"Shared"_s);
    EXPECT_EQ(detail::cleanAlias(u"Café"_s), u"Café"_s);
    const QString longAlias(300, u'x');
    EXPECT_EQ(detail::cleanAlias(longAlias).size(), 255);
    // A surrogate pair cut in half at the limit loses its first half too.
    const QString emoji = QString(254, u'x') + u"😀"_s;
    EXPECT_EQ(detail::cleanAlias(emoji), QString(254, u'x'));
}

TEST(ShareText, AliasesAreMadeUniqueCaseInsensitively)
{
    EXPECT_EQ(detail::uniqueAliases({u"Music"_s, u"Music"_s, u"music"_s, u"Other"_s}),
              QStringList({u"Music"_s, u"Music (2)"_s, u"music (3)"_s, u"Other"_s}));
    EXPECT_EQ(detail::uniqueAliases({u"Music (2)"_s, u"Music"_s, u"Music"_s}),
              QStringList({u"Music (2)"_s, u"Music"_s, u"Music (3)"_s}));
    EXPECT_EQ(detail::uniqueAliases({u""_s, u"."_s}), QStringList({u"Shared"_s, u"Shared (2)"_s}));
    // A placeholder in an alias is text, not a format.
    EXPECT_EQ(detail::uniqueAliases({u"%2"_s, u"%2"_s}), QStringList({u"%2"_s, u"%2 (2)"_s}));
}

// --- Words -----------------------------------------------------------------------------------------------------------

TEST(ShareText, WordsSplitAsRespondersSplitThem)
{
    EXPECT_EQ(detail::searchWords(u"Music\\Daft Punk - Discovery (2001)\\01 One More Time.flac"),
              QStringList({u"music"_s, u"daft"_s, u"punk"_s, u"discovery"_s, u"2001"_s, u"01"_s, u"one"_s, u"more"_s,
                           u"time"_s, u"flac"_s}));
    EXPECT_EQ(detail::searchWords(u"Ke$ha_Tik.Tok"), QStringList({u"ke"_s, u"ha"_s, u"tik"_s, u"tok"_s}));
    EXPECT_EQ(detail::searchWords(u"Don't [Stop]"), QStringList({u"don"_s, u"t"_s, u"stop"_s}));
    EXPECT_EQ(detail::searchWords(u"a–b—c"), QStringList({u"a"_s, u"b"_s, u"c"_s})); // en and em dashes
    EXPECT_EQ(detail::searchWords(u"AC＼DC"), QStringList({u"ac"_s, u"dc"_s}));     // the replaced separator splits
    EXPECT_TRUE(detail::searchWords(u" -_- ").isEmpty());
}

TEST(ShareText, WordsFoldCaseAndDiacritics)
{
    EXPECT_EQ(detail::foldWord(u"plain"), u"plain"_s);
    EXPECT_EQ(detail::searchWords(u"Röyksopp"), QStringList({u"royksopp"_s}));
    EXPECT_EQ(detail::searchWords(u"Röyksopp"_s.normalized(QString::NormalizationForm_D)),
              QStringList({u"royksopp"_s}));
    EXPECT_EQ(detail::searchWords(u"Beyoncé Sigur Rós"), QStringList({u"beyonce"_s, u"sigur"_s, u"ros"_s}));
    EXPECT_EQ(detail::searchWords(u"Mø Ærø Œuvre Straße Łódź Đorđe Þór Ħal"),
              QStringList({u"mo"_s, u"aero"_s, u"oeuvre"_s, u"strasse"_s, u"lodz"_s, u"dorde"_s, u"thor"_s,
                           u"hal"_s}));
    EXPECT_EQ(detail::searchWords(u"ﬁx ＦＵＬＬ"), QStringList({u"fix"_s, u"full"_s})); // compatibility forms
    EXPECT_EQ(detail::searchWords(u"Кино Ελλάδα"), QStringList({u"кино"_s, u"ελλαδα"_s}));
    EXPECT_EQ(detail::searchWords(u"日本語"), QStringList({u"日本語"_s}));
}

TEST(ShareText, CodePointsCountSurrogatePairsOnce)
{
    EXPECT_EQ(detail::codePointCount(u""), 0);
    EXPECT_EQ(detail::codePointCount(u"abc"), 3);
    EXPECT_EQ(detail::codePointCount(u"日本語"), 3);
    EXPECT_EQ(detail::codePointCount(u"😀😀"), 2);
}

} // namespace
