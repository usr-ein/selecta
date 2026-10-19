// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Zlib.h"

#include <QRandomGenerator>

#include <gtest/gtest.h>

namespace zlib = selecta::slsk::zlib;
using zlib::Error;

namespace {

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator generator(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (char &c : bytes) {
        c = static_cast<char>(generator.bounded(256));
    }
    return bytes;
}

} // namespace

TEST(Zlib, RoundTrips)
{
    const QByteArray text = QByteArray("Music\\Artist - Album\\01 Track.flac").repeated(1000);
    const QByteArray compressed = zlib::deflate(text);
    EXPECT_LT(compressed.size(), text.size());
    const zlib::Inflated inflated = zlib::inflate(compressed, text.size());
    ASSERT_TRUE(inflated.ok()) << zlib::errorName(inflated.error);
    EXPECT_EQ(inflated.data, text);
}

TEST(Zlib, RoundTripsIncompressibleAndEmptyData)
{
    const QByteArray noise = randomBytes(200'000, 7);
    EXPECT_EQ(zlib::inflate(zlib::deflate(noise), noise.size()).data, noise);
    const zlib::Inflated empty = zlib::inflate(zlib::deflate({}), 0);
    EXPECT_TRUE(empty.ok());
    EXPECT_TRUE(empty.data.isEmpty());
}

TEST(Zlib, EveryLevelProducesAStandardZlibStream)
{
    const QByteArray text = QByteArray("abc").repeated(500);
    for (int level = 0; level <= 9; ++level) {
        const QByteArray compressed = zlib::deflate(text, level);
        ASSERT_GE(compressed.size(), 2);
        // RFC 1950 header: CM = 8 (deflate), and (CMF * 256 + FLG) is a multiple of 31.
        const auto cmf = static_cast<quint32>(static_cast<quint8>(compressed.at(0)));
        const auto flg = static_cast<quint32>(static_cast<quint8>(compressed.at(1)));
        EXPECT_EQ(cmf & 0x0FU, 8U);
        EXPECT_EQ(((cmf << 8U) | flg) % 31U, 0U);
        EXPECT_EQ(zlib::inflate(compressed, text.size()).data, text);
    }
}

TEST(Zlib, OutputExactlyAtTheCapIsAccepted)
{
    const QByteArray data(1000, 'x');
    const zlib::Inflated inflated = zlib::inflate(zlib::deflate(data), 1000);
    EXPECT_TRUE(inflated.ok());
}

TEST(Zlib, OutputOneByteOverTheCapIsRefused)
{
    const QByteArray data(1001, 'x');
    const zlib::Inflated inflated = zlib::inflate(zlib::deflate(data), 1000);
    EXPECT_EQ(inflated.error, Error::TooLarge);
    EXPECT_TRUE(inflated.data.isEmpty());
}

TEST(Zlib, ZipBombStopsAtTheCap)
{
    // 64 MiB of zeros compresses to about 64 KiB; with a 1 MiB cap only about 1 MiB is ever produced.
    const QByteArray zeros(qsizetype{64} * 1024 * 1024, '\0');
    const QByteArray bomb = zlib::deflate(zeros, 9);
    EXPECT_LT(bomb.size(), 200 * 1024);
    const zlib::Inflated inflated = zlib::inflate(bomb, qsizetype{1024} * 1024);
    EXPECT_EQ(inflated.error, Error::TooLarge);
}

TEST(Zlib, TruncatedStreamIsRefused)
{
    const QByteArray compressed = zlib::deflate(randomBytes(10'000, 3));
    for (const qsizetype cut : {qsizetype{1}, qsizetype{2}, compressed.size() / 2, compressed.size() - 1}) {
        const zlib::Inflated inflated = zlib::inflate(compressed.left(cut), 1'000'000);
        EXPECT_EQ(inflated.error, Error::Truncated) << "cut at " << cut;
    }
}

TEST(Zlib, EmptyInputIsTruncated)
{
    EXPECT_EQ(zlib::inflate({}, 100).error, Error::Truncated);
}

TEST(Zlib, TrailingBytesAreRefused)
{
    QByteArray compressed = zlib::deflate("hello");
    compressed.append("junk");
    EXPECT_EQ(zlib::inflate(compressed, 100).error, Error::TrailingData);
}

TEST(Zlib, CorruptStreamIsRefused)
{
    EXPECT_EQ(zlib::inflate("not a zlib stream at all", 100).error, Error::Corrupt);
    // A valid header with a broken checksum.
    QByteArray compressed = zlib::deflate("hello world");
    const auto last = static_cast<quint8>(compressed.back());
    compressed.back() = static_cast<char>(last ^ 0x55U);
    EXPECT_EQ(zlib::inflate(compressed, 100).error, Error::Corrupt);
    // Raw deflate (no zlib header) and gzip are not accepted.
    QByteArray gzip = QByteArray::fromHex("1f8b0800000000000003");
    gzip.append(compressed);
    EXPECT_NE(zlib::inflate(gzip, 100).error, Error::None);
}

TEST(Zlib, NegativeCapIsRefused)
{
    EXPECT_EQ(zlib::inflate(zlib::deflate("x"), -1).error, Error::TooLarge);
}

TEST(Zlib, PrefixReadsOnlyTheStart)
{
    const QByteArray data = QByteArray("0123456789").repeated(100'000);
    const QByteArray compressed = zlib::deflate(data);
    const zlib::Inflated prefix = zlib::inflatePrefix(compressed, 25);
    ASSERT_TRUE(prefix.ok());
    EXPECT_EQ(prefix.data, data.left(25));
}

TEST(Zlib, PrefixOfAShortStreamIsTheWholeStream)
{
    const zlib::Inflated prefix = zlib::inflatePrefix(zlib::deflate("abc"), 100);
    ASSERT_TRUE(prefix.ok());
    EXPECT_EQ(prefix.data, QByteArray("abc"));
}

TEST(Zlib, PrefixIgnoresDamageAfterThePrefix)
{
    QByteArray compressed = zlib::deflate(randomBytes(50'000, 11));
    compressed.chop(10); // the end is gone; the start still inflates
    const zlib::Inflated prefix = zlib::inflatePrefix(compressed, 16);
    ASSERT_TRUE(prefix.ok()) << zlib::errorName(prefix.error);
    EXPECT_EQ(prefix.data.size(), 16);
}

TEST(Zlib, PrefixOfGarbageFails)
{
    EXPECT_FALSE(zlib::inflatePrefix("garbage!", 4).ok());
    EXPECT_FALSE(zlib::inflatePrefix({}, 4).ok());
}

TEST(Zlib, RandomGarbageNeverCrashes)
{
    for (quint32 seed = 0; seed < 300; ++seed) {
        const QByteArray garbage = randomBytes(1 + ((seed * 7) % 500), seed);
        (void)zlib::inflate(garbage, 4096);
        (void)zlib::inflatePrefix(garbage, 64);
    }
    SUCCEED();
}
