// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Frame.h"

#include "slsk/DistributedMessages.h"
#include "slsk/PeerMessages.h"
#include "slsk/ServerMessages.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;

TEST(Frame, EncodesLengthCodeAndPayload)
{
    EXPECT_EQ(encodeFrame(CodeWidth::Word, 26, "ab").toHex(' '), "06 00 00 00 1a 00 00 00 61 62");
    EXPECT_EQ(encodeFrame(CodeWidth::Byte, 3, "ab").toHex(' '), "03 00 00 00 03 61 62");
    EXPECT_EQ(encodeFrame(CodeWidth::Word, 32, {}).toHex(' '), "04 00 00 00 20 00 00 00");
}

TEST(Frame, MessageStructsPickTheirChannelsCodeWidth)
{
    EXPECT_EQ(frame(toserver::ServerPing{}).toHex(' '), "04 00 00 00 20 00 00 00");
    EXPECT_EQ(frame(peerinit::PierceFireWall{7}).toHex(' '), "05 00 00 00 00 07 00 00 00");
    EXPECT_EQ(frame(distrib::BranchLevel{2}).toHex(' '), "05 00 00 00 04 02 00 00 00");
    EXPECT_EQ(frame(peer::UserInfoRequest{}).toHex(' '), "04 00 00 00 0f 00 00 00");
}

TEST(FrameReader, ReadsFramesFedOneByteAtATime)
{
    const QByteArray stream = encodeFrame(CodeWidth::Word, 1, "hello") + encodeFrame(CodeWidth::Word, 2, {}) +
                              encodeFrame(CodeWidth::Word, 3, "x");
    FrameReader reader(CodeWidth::Word, 100);
    QList<Frame> frames;
    for (const char byte : stream) {
        reader.append(QByteArrayView(&byte, 1));
        while (std::optional<Frame> frame = reader.next()) {
            frames.append(*frame);
        }
    }
    ASSERT_EQ(frames.size(), 3);
    EXPECT_EQ(frames[0].code, 1U);
    EXPECT_EQ(frames[0].payload, QByteArray("hello"));
    EXPECT_EQ(frames[1].code, 2U);
    EXPECT_TRUE(frames[1].payload.isEmpty());
    EXPECT_EQ(frames[2].payload, QByteArray("x"));
    EXPECT_FALSE(reader.hasError());
    EXPECT_EQ(reader.buffered(), 0);
}

TEST(FrameReader, ReadsManyFramesFromOneChunk)
{
    QByteArray stream;
    for (int i = 0; i < 1000; ++i) {
        stream += encodeFrame(CodeWidth::Byte, static_cast<quint32>(i % 256), QByteArray::number(i));
    }
    FrameReader reader(CodeWidth::Byte, 16);
    reader.append(stream);
    int count = 0;
    while (std::optional<Frame> frame = reader.next()) {
        EXPECT_EQ(frame->payload, QByteArray::number(count));
        ++count;
    }
    EXPECT_EQ(count, 1000);
}

TEST(FrameReader, OversizeFrameIsRefusedBeforeItsPayloadArrives)
{
    FrameReader reader(CodeWidth::Word, 10);
    // Header only: length 4 + 11 announces an 11-byte payload.
    reader.append(QByteArray::fromHex("0f 00 00 00 01 00 00 00"));
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.error(), FrameReader::Error::Oversize);
    // Nothing more is accepted.
    reader.append(QByteArray(100, 'x'));
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.buffered(), 8);
}

TEST(FrameReader, FrameExactlyAtTheCapIsAccepted)
{
    FrameReader reader(CodeWidth::Word, 10);
    reader.append(encodeFrame(CodeWidth::Word, 1, QByteArray(10, 'a')));
    EXPECT_TRUE(reader.next().has_value());
    EXPECT_FALSE(reader.hasError());
}

TEST(FrameReader, HugeLengthFieldIsRefusedNotBuffered)
{
    FrameReader reader(CodeWidth::Word, qsizetype{16} * 1024 * 1024);
    reader.append(QByteArray::fromHex("ff ff ff ff 01 00 00 00"));
    EXPECT_EQ(reader.error(), FrameReader::Error::Oversize);
}

TEST(FrameReader, LengthShorterThanTheCodeIsAViolation)
{
    FrameReader word(CodeWidth::Word, 100);
    word.append(QByteArray::fromHex("03 00 00 00 01 00 00 00"));
    EXPECT_EQ(word.error(), FrameReader::Error::TooShort);
    FrameReader byte(CodeWidth::Byte, 100);
    byte.append(QByteArray::fromHex("00 00 00 00 01"));
    EXPECT_EQ(byte.error(), FrameReader::Error::TooShort);
}

TEST(FrameReader, LengthShorterThanTheCodeIsRefusedBeforeTheCodeArrives)
{
    // The length field alone says the frame is impossible: waiting for more bytes could wait forever.
    FrameReader word(CodeWidth::Word, 100);
    word.append(QByteArray::fromHex("02 00 00 00 01 00"));
    EXPECT_EQ(word.error(), FrameReader::Error::TooShort);
    EXPECT_FALSE(word.next().has_value());
    FrameReader byte(CodeWidth::Byte, 100);
    byte.append(QByteArray::fromHex("00 00 00 00"));
    EXPECT_EQ(byte.error(), FrameReader::Error::TooShort);

    // A possible length still waits for its code.
    FrameReader waiting(CodeWidth::Word, 100);
    waiting.append(QByteArray::fromHex("04 00 00 00 01 00"));
    EXPECT_EQ(waiting.error(), FrameReader::Error::None);
    EXPECT_FALSE(waiting.next().has_value());
    waiting.append(QByteArray::fromHex("00 00"));
    const std::optional<Frame> frame = waiting.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->code, 1U);
    EXPECT_TRUE(frame->payload.isEmpty());
}

TEST(FrameReader, CapFunctionDecidesPerCode)
{
    FrameReader reader(CodeWidth::Word, 0);
    reader.setCapFunction([](quint32 code) -> qsizetype {
        if (code == 5) {
            return -1; // unsolicited: refused outright
        }
        return code == 9 ? 1000 : 4;
    });
    reader.append(encodeFrame(CodeWidth::Word, 9, QByteArray(500, 'r')));
    ASSERT_TRUE(reader.next().has_value());
    reader.append(encodeFrame(CodeWidth::Word, 1, "abcd"));
    ASSERT_TRUE(reader.next().has_value());
    reader.append(encodeFrame(CodeWidth::Word, 5, {}));
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.error(), FrameReader::Error::Unsolicited);
}

TEST(FrameReader, EmptyPayloadUnderAZeroCap)
{
    FrameReader reader(CodeWidth::Word, 0);
    reader.append(encodeFrame(CodeWidth::Word, 4, {}));
    EXPECT_TRUE(reader.next().has_value());
    reader.append(encodeFrame(CodeWidth::Word, 4, "x"));
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.error(), FrameReader::Error::Oversize);
}

TEST(FrameReader, ViolationInTheNextFrameStillDeliversTheCurrentOne)
{
    FrameReader reader(CodeWidth::Word, 4);
    reader.append(QByteArray(encodeFrame(CodeWidth::Word, 1, "ok") + QByteArray::fromHex("ff 00 00 00 02 00 00 00")));
    const std::optional<Frame> first = reader.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->payload, QByteArray("ok"));
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_TRUE(reader.hasError());
}

TEST(FrameReader, TakeBufferedReturnsWhatFollowsAFrame)
{
    // A peer-init frame, then the raw 4-byte token of an F connection.
    FrameReader reader(CodeWidth::Byte, 100);
    reader.append(QByteArray(frame(peerinit::PeerInit{u"alice"_s, ConnectionType::File, 0}) +
                             QByteArray::fromHex("2a 00 00 00")));
    const std::optional<Frame> init = reader.next();
    ASSERT_TRUE(init.has_value());
    EXPECT_EQ(init->code, 1U);
    EXPECT_EQ(reader.takeBuffered(), QByteArray::fromHex("2a 00 00 00"));
    EXPECT_EQ(reader.buffered(), 0);
}

TEST(FrameReader, TakeBufferedReturnsAZeroTokenAndStartsOver)
{
    // A token of 0 after the peer-init frame reads as an impossible frame length: the bytes still come back whole.
    FrameReader reader(CodeWidth::Byte, 100);
    reader.append(QByteArray(frame(peerinit::PeerInit{u"alice"_s, ConnectionType::File, 0}) +
                             QByteArray::fromHex("00 00 00 00")));
    const std::optional<Frame> init = reader.next();
    ASSERT_TRUE(init.has_value());
    EXPECT_TRUE(reader.hasError());
    EXPECT_EQ(reader.takeBuffered(), QByteArray::fromHex("00 00 00 00"));
    EXPECT_FALSE(reader.hasError());
    reader.append(QByteArray::fromHex("00 00 00 00"));
    EXPECT_EQ(reader.takeBuffered(), QByteArray::fromHex("00 00 00 00"));
}

TEST(FrameReader, CompactsConsumedBytes)
{
    FrameReader reader(CodeWidth::Word, qsizetype{1024} * 1024);
    const QByteArray chunk = encodeFrame(CodeWidth::Word, 1, QByteArray(100'000, 'z'));
    for (int i = 0; i < 50; ++i) {
        reader.append(chunk);
        ASSERT_TRUE(reader.next().has_value());
    }
    EXPECT_EQ(reader.buffered(), 0);
}

TEST(FrameReader, ReportsTheAnnouncedSizeWhileAFrameArrives)
{
    FrameReader reader(CodeWidth::Word, 1000);
    const QByteArray full = encodeFrame(CodeWidth::Word, 5, QByteArray(600, 'p'));
    reader.append(full.left(100));
    EXPECT_EQ(reader.pendingFrameSize(), 600);
    EXPECT_FALSE(reader.next().has_value());
    reader.append(full.mid(100));
    EXPECT_TRUE(reader.next().has_value());
    EXPECT_EQ(reader.pendingFrameSize(), 0);
}
