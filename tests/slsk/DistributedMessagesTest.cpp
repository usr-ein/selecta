// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/DistributedMessages.h"

#include "fixtures/Samples.h"
#include "slsk/Frame.h"

#include <QRandomGenerator>
#include <QSet>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using selecta::slsk::testing::codeOf;
using selecta::slsk::testing::distribSamples;
using selecta::slsk::testing::payloadOf;

TEST(DistributedMessages, GoldenSearch)
{
    const distrib::Search search{49, u"carol"_s, 0x01020304, u"abc"_s};
    EXPECT_EQ(search.encode().toHex(' '), "31 00 00 00 05 00 00 00 63 61 72 6f 6c 04 03 02 01 03 00 00 00 61 62 63");
    EXPECT_EQ(frame(search).left(5).toHex(' '), "19 00 00 00 03");
}

TEST(DistributedMessages, SearchWithAnotherIdentifierIsRejected)
{
    const distrib::Search search{0, u"x"_s, 1, u"q"_s}; // Soulseek.NET's builder writes 0
    EXPECT_FALSE(distrib::Search::decode(search.encode()).has_value());
    EXPECT_EQ(decodeDistrib(3, search.encode()).status, DecodeStatus::Malformed);
}

TEST(DistributedMessages, NegativeBranchLevelDecodes)
{
    // Decoding keeps the value; DistributedNode is the one that rejects it.
    const std::optional<distrib::BranchLevel> level = distrib::BranchLevel::decode(QByteArray::fromHex("ff ff ff ff"));
    ASSERT_TRUE(level.has_value());
    EXPECT_EQ(level->level, -1);
}

TEST(DistributedMessages, SamplesCoverEveryMessageAndRoundTrip)
{
    QSet<std::size_t> covered;
    const QList<DistribMessage> allDistribSamples = distribSamples();
    for (const DistribMessage &sample : allDistribSamples) {
        covered.insert(sample.index());
        std::visit(
            [](const auto &message) {
                using M = std::decay_t<decltype(message)>;
                const std::optional<M> decoded = M::decode(message.encode());
                ASSERT_TRUE(decoded.has_value());
                EXPECT_EQ(*decoded, message);
                const Decoded<DistribMessage> dispatched =
                    decodeDistrib(static_cast<quint32>(M::kCode), message.encode());
                ASSERT_EQ(dispatched.status, DecodeStatus::Decoded);
                EXPECT_TRUE(std::holds_alternative<M>(*dispatched.message));
            },
            sample);
    }
    EXPECT_EQ(static_cast<std::size_t>(covered.size()), std::variant_size_v<DistribMessage>);
}

TEST(DistributedMessages, EmbeddedSearchUnpacks)
{
    const distrib::Search inner{49, u"searcher"_s, 5, u"query"_s};
    const distrib::EmbeddedMessage outer{3, inner.encode()};
    const std::optional<distrib::EmbeddedMessage> decoded = distrib::EmbeddedMessage::decode(outer.encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->distribCode, 3);
    EXPECT_EQ(distrib::Search::decode(decoded->payload), inner);
}

TEST(DistributedMessages, PingAndChildDepthAreIgnored)
{
    EXPECT_EQ(decodeDistrib(0, {}).status, DecodeStatus::Ignored);
    EXPECT_EQ(decodeDistrib(7, QByteArray(4, '\0')).status, DecodeStatus::Ignored);
    EXPECT_EQ(decodeDistrib(9, {}).status, DecodeStatus::Unknown);
    EXPECT_EQ(decodeDistrib(256 + 3, {}).status, DecodeStatus::Unknown); // not a uint8 code
}

TEST(DistributedMessages, TruncatedAndRandomPayloadsNeverCrash)
{
    const QList<DistribMessage> allDistribSamples = distribSamples();
    for (const DistribMessage &sample : allDistribSamples) {
        const QByteArray payload = payloadOf(sample);
        for (qsizetype cut = 0; cut < payload.size(); ++cut) {
            (void)decodeDistrib(codeOf(sample), payload.left(cut));
        }
    }
    QRandomGenerator random(5);
    for (int i = 0; i < 2000; ++i) {
        QByteArray payload(random.bounded(40), Qt::Uninitialized);
        for (char &c : payload) {
            c = static_cast<char>(random.bounded(256));
        }
        (void)decodeDistrib(random.bounded(100U), payload);
    }
    SUCCEED();
}

TEST(DistributedMessages, TruncatedSearchIsMalformed)
{
    const QByteArray payload = distrib::Search{49, u"a"_s, 1, u"b"_s}.encode();
    for (qsizetype cut = 0; cut < payload.size(); ++cut) {
        EXPECT_EQ(decodeDistrib(3, payload.left(cut)).status, DecodeStatus::Malformed) << cut;
    }
}
