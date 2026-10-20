// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ImageHeader.h"

#include "meta/support/Images.h"

#include <QRandomGenerator>

#include <array>
#include <gtest/gtest.h>

using namespace selecta::meta::detail;
using namespace selecta::meta::testing;

TEST(ImageHeader, BaselineJpeg)
{
    const auto info = readImageHeader(jpeg(640, 480));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->format, ImageFormat::Jpeg);
    EXPECT_EQ(info->width, 640U);
    EXPECT_EQ(info->height, 480U);
}

TEST(ImageHeader, ProgressiveAndOtherFrameTypes)
{
    for (const unsigned char sof : {0xC1, 0xC2, 0xC3, 0xC5, 0xC9, 0xCB, 0xCD, 0xCF}) {
        const auto info = readImageHeader(jpeg(1200, 1200, sof));
        ASSERT_TRUE(info.has_value()) << static_cast<int>(sof);
        EXPECT_EQ(info->width, 1200U);
    }
}

TEST(ImageHeader, NonFrameMarkersInTheSofRangeAreSkipped)
{
    // DHT (C4), JPG (C8) and DAC (CC) are not frame headers: a stream with only those and then EOI has no size.
    QByteArray bytes("\xFF\xD8", 2);
    bytes += jpegSegment(0xC4, QByteArray(20, '\x01'));
    bytes += jpegSegment(0xCC, QByteArray(4, '\x01'));
    bytes += QByteArray("\xFF\xD9", 2);
    EXPECT_FALSE(readImageHeader(bytes).has_value());
}

TEST(ImageHeader, LargeExifSegmentBeforeTheFrame)
{
    const auto info = readImageHeader(jpeg(800, 600, 0xC0, 60'000));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 800U);
    EXPECT_EQ(info->height, 600U);
}

TEST(ImageHeader, FillBytesAndStandaloneMarkersAreTolerated)
{
    QByteArray bytes("\xFF\xD8", 2);
    bytes += QByteArray("\xFF\xFF\xFF", 3); // fill bytes before a marker
    bytes += jpegSegment(0xE0, QByteArray(14, '\0'));
    bytes += QByteArray("\xFF\x01", 2); // TEM: no length
    bytes += jpeg(32, 16).mid(2 + 18);  // the SOF and the rest of a valid file
    const auto info = readImageHeader(bytes);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->width, 32U);
    EXPECT_EQ(info->height, 16U);
}

TEST(ImageHeader, MalformedJpegsAreRejected)
{
    const QByteArray good = jpeg(640, 640);
    // Truncated anywhere before the end of the frame header.
    const qsizetype frameEnd = good.indexOf("\xFF\xDA");
    for (qsizetype cut = 0; cut < frameEnd; ++cut) {
        EXPECT_FALSE(readImageHeader(good.left(cut)).has_value()) << cut;
    }
    // A segment length below 2 (zero-length segment).
    QByteArray zeroLength("\xFF\xD8\xFF\xE0\x00\x00", 6);
    zeroLength += good.mid(2);
    EXPECT_FALSE(readImageHeader(zeroLength).has_value());
    const QByteArray lengthOne("\xFF\xD8\xFF\xE0\x00\x01", 6);
    EXPECT_FALSE(readImageHeader(lengthOne).has_value());
    // A length running past the end of the data.
    QByteArray oversized("\xFF\xD8\xFF\xE0\x7F\xFF", 6);
    oversized += QByteArray(100, '\0');
    EXPECT_FALSE(readImageHeader(oversized).has_value());
    // A scan before any frame header.
    QByteArray scanFirst("\xFF\xD8", 2);
    scanFirst += jpegSegment(0xDA, QByteArray(10, '\0'));
    scanFirst += good.mid(2);
    EXPECT_FALSE(readImageHeader(scanFirst).has_value());
    // End of image before any frame header.
    EXPECT_FALSE(readImageHeader(QByteArray("\xFF\xD8\xFF\xD9", 4)).has_value());
    // A byte that isn't a marker where one must be.
    QByteArray noMarker("\xFF\xD8", 2);
    noMarker += jpegSegment(0xE0, QByteArray(14, '\0'));
    noMarker += QByteArray("\x12\x34", 2);
    noMarker += good.mid(2);
    EXPECT_FALSE(readImageHeader(noMarker).has_value());
    // A stuffed zero after FF outside entropy data.
    QByteArray stuffed("\xFF\xD8\xFF\x00", 4);
    stuffed += good.mid(2);
    EXPECT_FALSE(readImageHeader(stuffed).has_value());
    // A frame header too short to hold the size.
    QByteArray shortFrame("\xFF\xD8", 2);
    shortFrame += jpegSegment(0xC0, QByteArray("\x08\x01\x00", 3));
    EXPECT_FALSE(readImageHeader(shortFrame).has_value());
    // Zero height (defined later by DNL) and zero width.
    EXPECT_FALSE(readImageHeader(jpeg(640, 0)).has_value());
    EXPECT_FALSE(readImageHeader(jpeg(0, 640)).has_value());
    // Only fill bytes after SOI.
    EXPECT_FALSE(readImageHeader(QByteArray("\xFF\xD8\xFF\xFF\xFF\xFF", 6)).has_value());
}

TEST(ImageHeader, TheFrameMustStartWithinTheScanBudget)
{
    QByteArray bytes("\xFF\xD8", 2);
    const QByteArray filler = jpegSegment(0xE2, QByteArray(65'000, 'i'));
    while (bytes.size() < kMaxScanBytes + 1000) {
        bytes += filler;
    }
    bytes += jpeg(100, 100).mid(2);
    EXPECT_FALSE(readImageHeader(bytes).has_value());
}

TEST(ImageHeader, TooManySegmentsStopTheWalk)
{
    QByteArray bytes("\xFF\xD8", 2);
    for (int i = 0; i < kMaxSegments + 10; ++i) {
        bytes += jpegSegment(0xFE, QByteArray()); // empty comments
    }
    bytes += jpeg(100, 100).mid(2);
    EXPECT_FALSE(readImageHeader(bytes).has_value());
}

TEST(ImageHeader, Png)
{
    const auto info = readImageHeader(png(1000, 1000));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->format, ImageFormat::Png);
    EXPECT_EQ(info->width, 1000U);
    EXPECT_EQ(info->height, 1000U);
    const auto huge = readImageHeader(png(60'000, 2));
    ASSERT_TRUE(huge.has_value()); // reported as is; the size bound is the caller's decision
    EXPECT_EQ(huge->width, 60'000U);
}

TEST(ImageHeader, MalformedPngsAreRejected)
{
    const QByteArray good = png(500, 500);
    for (qsizetype cut = 0; cut < 24; ++cut) {
        EXPECT_FALSE(readImageHeader(good.left(cut)).has_value()) << cut;
    }
    QByteArray wrongLength = good;
    wrongLength[11] = '\x0E'; // IHDR length 14
    EXPECT_FALSE(readImageHeader(wrongLength).has_value());
    QByteArray notIhdr = good;
    notIhdr[12] = 'X';
    EXPECT_FALSE(readImageHeader(notIhdr).has_value());
    EXPECT_FALSE(readImageHeader(png(0, 10)).has_value());
    EXPECT_FALSE(readImageHeader(png(10, 0)).has_value());
    EXPECT_FALSE(readImageHeader(png(0x80000000U, 10)).has_value()); // above 2^31 − 1
}

TEST(ImageHeader, OtherFormatsAreUnknown)
{
    EXPECT_FALSE(readImageHeader(QByteArray("GIF89a\x10\x00\x10\x00", 10)).has_value());
    EXPECT_FALSE(readImageHeader(QByteArray("<html><body>Not found</body></html>")).has_value());
    EXPECT_FALSE(readImageHeader(QByteArray()).has_value());
    EXPECT_FALSE(readImageHeader(QByteArray("\xFF\xD8", 2)).has_value());
}

// Random bytes behind valid and invalid prefixes must never read out of bounds (run under ASan in CI).
TEST(ImageHeader, RandomBytesNeverCrash)
{
    QRandomGenerator random(20261020);
    const std::array<QByteArray, 3> prefixes = {QByteArray("\xFF\xD8\xFF", 3), QByteArray("\x89PNG\r\n\x1a\n", 8),
                                                QByteArray()};
    int images = 0;
    for (int round = 0; round < 20'000; ++round) {
        QByteArray bytes = prefixes.at(static_cast<std::size_t>(round % 3));
        const int length = random.bounded(0, 300);
        for (int i = 0; i < length; ++i) {
            // Bias towards 0xFF and the SOF range to reach the marker and length paths often.
            const quint32 pick = random.bounded(8U);
            int value = random.bounded(256);
            if (pick == 0) {
                value = 0xFF;
            } else if (pick == 1) {
                value = random.bounded(0xC0, 0xDF);
            }
            bytes += static_cast<char>(value);
        }
        const auto info = readImageHeader(bytes);
        if (info.has_value()) {
            ++images;
            EXPECT_GT(info->width, 0U);
            EXPECT_GT(info->height, 0U);
        }
    }
    // Mutations of a valid file: flip bytes one by one.
    const QByteArray good = jpeg(300, 200, 0xC2, 100);
    for (qsizetype i = 0; i < good.size(); ++i) {
        for (const char flip : {'\x00', '\xFF', '\x80'}) {
            QByteArray mutated = good;
            mutated[i] = flip;
            (void)readImageHeader(mutated);
        }
    }
    SUCCEED() << images << " random buffers parsed as images";
}
