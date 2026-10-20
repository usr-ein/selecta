// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Codec.h"

#include <gtest/gtest.h>
#include <limits>

using namespace Qt::StringLiterals;
using selecta::slsk::decodeText;
using selecta::slsk::isValidUtf8;
using selecta::slsk::md5Hex;
using selecta::slsk::Reader;
using selecta::slsk::Writer;

TEST(Codec, WritesLittleEndianIntegersOfEveryWidth)
{
    Writer w;
    w.u8(0x01);
    w.u16(0x0302);
    w.u32(0x07060504);
    w.i32(-2);
    w.u64(0x0F0E0D0C0B0A0908ULL);
    w.boolean(true);
    w.boolean(false);
    EXPECT_EQ(w.data().toHex(' '), "01 02 03 04 05 06 07 fe ff ff ff 08 09 0a 0b 0c 0d 0e 0f 01 00");
}

TEST(Codec, ReadsWhatItWrites)
{
    Writer w;
    w.u8(200);
    w.u16(65000);
    w.u32(4'000'000'000U);
    w.i32(std::numeric_limits<qint32>::min());
    w.u64(std::numeric_limits<quint64>::max());
    w.boolean(true);
    w.string(u"héllo"_s);
    w.string(QByteArrayView("\xff\xfe raw"));
    const QByteArray bytes = w.take();

    Reader r(bytes);
    EXPECT_EQ(r.u8(), 200);
    EXPECT_EQ(r.u16(), 65000);
    EXPECT_EQ(r.u32(), 4'000'000'000U);
    EXPECT_EQ(r.i32(), std::numeric_limits<qint32>::min());
    EXPECT_EQ(r.u64(), std::numeric_limits<quint64>::max());
    EXPECT_TRUE(r.boolean());
    EXPECT_EQ(decodeText(r.string()), u"héllo"_s);
    EXPECT_EQ(r.string(), QByteArray("\xff\xfe raw"));
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.atEnd());
}

TEST(Codec, StringIsLengthPrefixedWithoutTerminator)
{
    Writer w;
    w.string(u"abc"_s);
    EXPECT_EQ(w.data().toHex(' '), "03 00 00 00 61 62 63");
}

TEST(Codec, AnyNonZeroByteIsTrue)
{
    const QByteArray bytes = QByteArray::fromHex("00 01 02 ff");
    Reader r(bytes);
    EXPECT_FALSE(r.boolean());
    EXPECT_TRUE(r.boolean());
    EXPECT_TRUE(r.boolean());
    EXPECT_TRUE(r.boolean());
}

TEST(Codec, ReadingPastTheEndFailsAndStaysFailed)
{
    const QByteArray bytes = QByteArray::fromHex("01 02 03");
    Reader r(bytes);
    EXPECT_EQ(r.u32(), 0U); // only 3 bytes
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.u8(), 0); // sticky: even a read that would fit now returns zero
    EXPECT_EQ(r.remaining(), 0);
    EXPECT_TRUE(r.rest().isEmpty());
}

TEST(Codec, StringLongerThanTheMessageFailsWithoutAllocating)
{
    // Length 0xFFFFFFFF, then two bytes.
    const QByteArray bytes = QByteArray::fromHex("ff ff ff ff 61 62");
    Reader r(bytes);
    EXPECT_TRUE(r.string().isEmpty());
    EXPECT_FALSE(r.ok());
}

TEST(Codec, StringExactlyFillingTheRestIsFine)
{
    const QByteArray bytes = QByteArray::fromHex("02 00 00 00 61 62");
    Reader r(bytes);
    EXPECT_EQ(r.string(), QByteArray("ab"));
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.atEnd());
}

TEST(Codec, EmptyString)
{
    const QByteArray bytes = QByteArray::fromHex("00 00 00 00");
    Reader r(bytes);
    EXPECT_TRUE(r.string().isEmpty());
    EXPECT_TRUE(r.ok());
}

TEST(Codec, CountIsCheckedAgainstTheBytesThatRemain)
{
    // A count of 3 items of at least 4 bytes needs 12 bytes; 8 remain.
    const QByteArray bytes = QByteArray::fromHex("03 00 00 00 00 00 00 00 00 00 00 00");
    Reader r(bytes);
    EXPECT_EQ(r.count(4), 0);
    EXPECT_FALSE(r.ok());

    Reader fits(bytes);
    EXPECT_EQ(fits.count(2), 3); // 3 × 2 ≤ 8
    EXPECT_TRUE(fits.ok());
}

TEST(Codec, HugeCountFailsInsteadOfOverflowing)
{
    const QByteArray bytes = QByteArray::fromHex("ff ff ff ff 00");
    Reader r(bytes);
    EXPECT_EQ(r.count(std::numeric_limits<qsizetype>::max()), 0);
    EXPECT_FALSE(r.ok());
    Reader zeroSized(bytes);
    EXPECT_EQ(zeroSized.count(0), 0); // a zero item size is treated as one byte: 2^32-1 items can't fit in 1 byte
    EXPECT_FALSE(zeroSized.ok());
}

TEST(Codec, RestAndSkip)
{
    const QByteArray bytes = QByteArray::fromHex("01 02 03 04 05");
    Reader r(bytes);
    r.skip(2);
    EXPECT_EQ(r.rest(), QByteArrayView(QByteArray::fromHex("03 04 05")));
    EXPECT_TRUE(r.atEnd());
    r.skip(1);
    EXPECT_FALSE(r.ok());
}

TEST(Codec, NegativeByteCountFails)
{
    const QByteArray bytes = QByteArray::fromHex("01 02");
    Reader r(bytes);
    EXPECT_TRUE(r.bytes(-1).isEmpty());
    EXPECT_FALSE(r.ok());
}

TEST(Codec, DecodesUtf8)
{
    EXPECT_EQ(decodeText(QByteArray("Bj\xc3\xb6rk")), u"Björk"_s);
    EXPECT_EQ(decodeText(QByteArray("\xe6\x97\xa5\xe6\x9c\xac")), u"日本"_s);
    EXPECT_EQ(decodeText(QByteArray("\xf0\x9f\x8e\xb5")), u"🎵"_s);
}

TEST(Codec, FallsBackToLatin1ForInvalidUtf8)
{
    // "Björk" in Latin-1: 0xF6 alone is not valid UTF-8.
    EXPECT_EQ(decodeText(QByteArray("Bj\xf6rk")), u"Björk"_s);
    EXPECT_FALSE(isValidUtf8(QByteArray("Bj\xf6rk")));
    // Overlong encodings and lone surrogates are invalid UTF-8 too.
    EXPECT_FALSE(isValidUtf8(QByteArray("\xc0\xaf")));
    EXPECT_FALSE(isValidUtf8(QByteArray("\xed\xa0\x80")));
    EXPECT_EQ(decodeText(QByteArray("\xc0\xaf")), QString::fromLatin1("\xc0\xaf"));
}

TEST(Codec, KeepsALeadingByteOrderMark)
{
    const QString decoded = decodeText(QByteArray("\xef\xbb\xbfname"));
    ASSERT_EQ(decoded.size(), 5);
    EXPECT_EQ(decoded.at(0), QChar(0xFEFF));
}

TEST(Codec, EmbeddedNulIsKept)
{
    const QString decoded = decodeText(QByteArray("a\0b", 3));
    EXPECT_EQ(decoded.size(), 3);
}

TEST(Codec, Md5HexIsLowerCase)
{
    EXPECT_EQ(md5Hex("usernamepassword"), QByteArray("d51c9a7e9353746a6020f9602d452929"));
    EXPECT_EQ(md5Hex(""), QByteArray("d41d8cd98f00b204e9800998ecf8427e"));
}
