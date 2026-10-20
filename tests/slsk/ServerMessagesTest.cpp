// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/ServerMessages.h"

#include "fixtures/Samples.h"
#include "slsk/Frame.h"

#include <QRandomGenerator>
#include <QSet>

#include <gtest/gtest.h>
#include <string_view>
#include <variant>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using selecta::slsk::testing::codeOf;
using selecta::slsk::testing::fromServerSamples;
using selecta::slsk::testing::payloadOf;
using selecta::slsk::testing::toServerSamples;

namespace {

template <typename Variant>
QSet<std::size_t> coveredAlternatives(const QList<Variant> &samples)
{
    QSet<std::size_t> covered;
    for (const Variant &sample : samples) {
        covered.insert(sample.index());
    }
    return covered;
}

template <typename Variant, typename DecodeFn>
void expectRoundTrips(const QList<Variant> &samples, DecodeFn decodeByCode)
{
    for (const Variant &sample : samples) {
        std::visit(
            [&](const auto &message) {
                using M = std::decay_t<decltype(message)>;
                const QByteArray payload = message.encode();
                const std::optional<M> decoded = M::decode(payload);
                ASSERT_TRUE(decoded.has_value()) << "code " << static_cast<quint32>(M::kCode);
                EXPECT_EQ(*decoded, message) << "code " << static_cast<quint32>(M::kCode);
                // Re-encoding gives the same bytes: nothing lost, nothing invented.
                EXPECT_EQ(decoded->encode(), payload);
                // Dispatch by code finds the same type.
                const auto dispatched = decodeByCode(static_cast<quint32>(M::kCode), payload);
                ASSERT_EQ(dispatched.status, DecodeStatus::Decoded);
                ASSERT_TRUE(std::holds_alternative<M>(*dispatched.message));
                EXPECT_EQ(std::get<M>(*dispatched.message), message);
            },
            sample);
    }
}

QByteArray randomBytes(QRandomGenerator &random, qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    for (char &c : bytes) {
        c = static_cast<char>(random.bounded(256));
    }
    return bytes;
}

} // namespace

// --- golden vectors --------------------------------------------------------------------------------------------------

TEST(ServerMessages, LoginMatchesTheNicotineDocumentExample)
{
    const toserver::Login login = toserver::Login::make(u"username"_s, u"password"_s);
    EXPECT_EQ(login.majorVersion, 177U);
    EXPECT_EQ(login.minorVersion, 1U);
    EXPECT_EQ(login.hash, QByteArray("d51c9a7e9353746a6020f9602d452929"));
    const QByteArray expected = QByteArray::fromHex(
        "48 00 00 00 01 00 00 00 08 00 00 00 75 73 65 72 6e 61 6d 65 08 00 00 00 70 61 73 73 77 6f 72 64"
        "b1 00 00 00 20 00 00 00 64 35 31 63 39 61 37 65 39 33 35 33 37 34 36 61 36 30 32 30 66 39 36 30"
        "32 64 34 35 32 39 32 39 01 00 00 00");
    EXPECT_EQ(expected.size(), 76);
    EXPECT_EQ(frame(login), expected);
}

TEST(ServerMessages, MessagesKnowTheirNames)
{
    // For logs: the struct's own name, whatever the code says.
    static_assert(std::string_view(toserver::Login::kName) == "Login");
    static_assert(std::string_view(fromserver::LoginReply::kName) == "LoginReply");
    static_assert(std::string_view(fromserver::SearchRequest::kName) == "SearchRequest");
    QSet<QString> names;
    const QList<ToServerMessage> samples = toServerSamples();
    for (const ToServerMessage &sample : samples) {
        names.insert(
            std::visit([](const auto &m) { return QString::fromLatin1(std::decay_t<decltype(m)>::kName); }, sample));
    }
    EXPECT_EQ(names.size(), static_cast<qsizetype>(std::variant_size_v<ToServerMessage>));
}

TEST(ServerMessages, IdentityIsTheExperimentalClientNumber)
{
    EXPECT_EQ(protocol::kMajorVersion, 177U);
    for (const quint32 reserved : {157U, 158U, 159U, 160U, 169U, 170U, 175U, 176U}) {
        EXPECT_NE(protocol::kMajorVersion, reserved);
    }
    EXPECT_NE(protocol::kMinorVersion, 0U);
}

TEST(ServerMessages, GoldenGetPeerAddressRequestAndReply)
{
    EXPECT_EQ(frame(toserver::GetPeerAddress{u"bob"_s}).toHex(' '), "0b 00 00 00 03 00 00 00 03 00 00 00 62 6f 62");
    // ip 1.2.3.4 travels as the bytes 04 03 02 01; port 2234 = 0x08ba.
    const QByteArray reply = QByteArray::fromHex("03 00 00 00 62 6f 62 04 03 02 01 ba 08 00 00");
    const std::optional<fromserver::PeerAddress> address = fromserver::PeerAddress::decode(reply);
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->username, u"bob"_s);
    EXPECT_EQ(ipv4Address(address->ip), QHostAddress(u"1.2.3.4"_s));
    EXPECT_EQ(address->port, 2234U);
    EXPECT_FALSE(address->obfuscationType.has_value());
}

TEST(ServerMessages, GoldenConnectToPeer)
{
    EXPECT_EQ(toserver::ConnectToPeer({1, u"bob"_s, ConnectionType::Peer}).encode().toHex(' '),
              "01 00 00 00 03 00 00 00 62 6f 62 01 00 00 00 50");
    const QByteArray relayed = QByteArray::fromHex("03 00 00 00 62 6f 62 01 00 00 00 46 04 03 02 01 ba 08 00 00 "
                                                   "2a 00 00 00 01");
    const std::optional<fromserver::ConnectToPeerRequest> request = fromserver::ConnectToPeerRequest::decode(relayed);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->username, u"bob"_s);
    EXPECT_EQ(request->type, ConnectionType::File);
    EXPECT_EQ(ipv4Address(request->ip), QHostAddress(u"1.2.3.4"_s));
    EXPECT_EQ(request->token, 42U);
    EXPECT_TRUE(request->privileged);
}

TEST(ServerMessages, GoldenSearchAndStatus)
{
    EXPECT_EQ(toserver::FileSearch({0x0102, u"ab"_s}).encode().toHex(' '), "02 01 00 00 02 00 00 00 61 62");
    EXPECT_EQ(toserver::SetStatus{2}.encode().toHex(' '), "02 00 00 00");
    const toserver::SetWaitPort port{2234, std::nullopt, std::nullopt};
    EXPECT_EQ(port.encode().toHex(' '), "ba 08 00 00");
    EXPECT_EQ(toserver::HaveNoParent{true}.encode().toHex(' '), "01");
}

TEST(ServerMessages, DecodesALoginRejectionWithDetail)
{
    const QByteArray payload = QByteArray::fromHex("00 0f 00 00 00") + "INVALIDUSERNAME" +
                               QByteArray::fromHex("0e 00 00 00") + "Nick too long.";
    const std::optional<fromserver::LoginReply> reply = fromserver::LoginReply::decode(payload);
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->success);
    EXPECT_EQ(reply->reason, u"INVALIDUSERNAME"_s);
    EXPECT_EQ(reply->detail, u"Nick too long."_s);
}

TEST(ServerMessages, LoginSuccessWithoutOptionalTail)
{
    // Greeting and IP only: the password hash and supporter flag are optional trailing fields.
    const QByteArray payload = QByteArray::fromHex("01 02 00 00 00 68 69 01 00 00 7f");
    const std::optional<fromserver::LoginReply> reply = fromserver::LoginReply::decode(payload);
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->success);
    EXPECT_EQ(reply->greeting, u"hi"_s);
    EXPECT_EQ(ipv4Address(reply->ownIp), QHostAddress(u"127.0.0.1"_s));
    EXPECT_TRUE(reply->passwordHash.isEmpty());
    EXPECT_FALSE(reply->isSupporter);
}

// --- round trips -----------------------------------------------------------------------------------------------------

TEST(ServerMessages, SamplesCoverEveryMessage)
{
    EXPECT_EQ(static_cast<std::size_t>(coveredAlternatives(toServerSamples()).size()),
              std::variant_size_v<ToServerMessage>);
    EXPECT_EQ(static_cast<std::size_t>(coveredAlternatives(fromServerSamples()).size()),
              std::variant_size_v<FromServerMessage>);
}

TEST(ServerMessages, EveryClientMessageRoundTrips)
{
    expectRoundTrips(toServerSamples(), decodeToServer);
}

TEST(ServerMessages, EveryServerMessageRoundTrips)
{
    expectRoundTrips(fromServerSamples(), decodeFromServer);
}

TEST(ServerMessages, ExtraTrailingBytesAreIgnored)
{
    // Four bytes that don't parse as any optional trailing field (a string length of 0xefbeadde): ignored, never a
    // reason to drop the message.
    const QList<FromServerMessage> allFromServerSamples = fromServerSamples();
    for (const FromServerMessage &sample : allFromServerSamples) {
        if (std::holds_alternative<fromserver::EmbeddedMessage>(sample)) {
            continue; // its last field is "the rest of the payload"
        }
        const QByteArray padded = payloadOf(sample) + QByteArray::fromHex("de ad be ef");
        const Decoded<FromServerMessage> decoded = decodeFromServer(codeOf(sample), padded);
        ASSERT_EQ(decoded.status, DecodeStatus::Decoded) << codeOf(sample);
        EXPECT_EQ(*decoded.message, sample) << codeOf(sample);
    }
}

// --- truncation and garbage ------------------------------------------------------------------------------------------

TEST(ServerMessages, TruncatedPayloadsNeverCrash)
{
    const QList<FromServerMessage> allFromServerSamples = fromServerSamples();
    for (const FromServerMessage &sample : allFromServerSamples) {
        const QByteArray payload = payloadOf(sample);
        for (qsizetype cut = 0; cut < payload.size(); ++cut) {
            (void)decodeFromServer(codeOf(sample), payload.left(cut));
        }
    }
    const QList<ToServerMessage> allToServerSamples = toServerSamples();
    for (const ToServerMessage &sample : allToServerSamples) {
        const QByteArray payload = payloadOf(sample);
        for (qsizetype cut = 0; cut < payload.size(); ++cut) {
            (void)decodeToServer(codeOf(sample), payload.left(cut));
        }
    }
    SUCCEED();
}

TEST(ServerMessages, TruncatedRequiredFieldsAreMalformed)
{
    const auto expectAllPrefixesMalformed = [](quint32 code, const QByteArray &payload, bool toServer) {
        for (qsizetype cut = 0; cut < payload.size(); ++cut) {
            const DecodeStatus status = toServer ? decodeToServer(code, payload.left(cut)).status
                                                 : decodeFromServer(code, payload.left(cut)).status;
            EXPECT_EQ(status, DecodeStatus::Malformed) << "code " << code << " cut " << cut;
        }
    };
    expectAllPrefixesMalformed(1, toserver::Login::make(u"u"_s, u"p"_s).encode(), true);
    expectAllPrefixesMalformed(18, toserver::ConnectToPeer{5, u"bob"_s, ConnectionType::Peer}.encode(), true);
    expectAllPrefixesMalformed(26, fromserver::SearchRequest{u"a"_s, 1, u"q"_s}.encode(), false);
    expectAllPrefixesMalformed(36, fromserver::UserStatsUpdate{u"a"_s, {}}.encode(), false);
    expectAllPrefixesMalformed(102, fromserver::PossibleParents{{{u"p"_s, 1, 2}}}.encode(), false);
    expectAllPrefixesMalformed(160, fromserver::ExcludedSearchPhrases{{u"x"_s}}.encode(), false);
}

TEST(ServerMessages, RandomPayloadsNeverCrash)
{
    QRandomGenerator random(2026);
    for (int i = 0; i < 3000; ++i) {
        const quint32 code = random.bounded(1100U);
        const QByteArray payload = randomBytes(random, random.bounded(64));
        (void)decodeFromServer(code, payload);
        (void)decodeToServer(code, payload);
    }
    SUCCEED();
}

TEST(ServerMessages, MutatedSamplesNeverCrash)
{
    QRandomGenerator random(7);
    const QList<FromServerMessage> samples = fromServerSamples();
    for (int round = 0; round < 40; ++round) {
        for (const FromServerMessage &sample : samples) {
            QByteArray payload = payloadOf(sample);
            if (payload.isEmpty()) {
                continue;
            }
            const int flips = 1 + static_cast<int>(random.bounded(4U));
            for (int f = 0; f < flips; ++f) {
                payload[random.bounded(static_cast<int>(payload.size()))] = static_cast<char>(random.bounded(256));
            }
            (void)decodeFromServer(codeOf(sample), payload);
        }
    }
    SUCCEED();
}

// --- dispatch --------------------------------------------------------------------------------------------------------

TEST(ServerMessages, KnownButUndecodedCodesAreIgnored)
{
    for (const quint32 code : {11U, 12U, 32U, 33U, 40U, 54U, 113U, 114U, 133U, 148U, 152U, 153U}) {
        EXPECT_EQ(decodeFromServer(code, QByteArray("whatever")).status, DecodeStatus::Ignored) << code;
    }
}

TEST(ServerMessages, UnknownCodesAreUnknown)
{
    for (const quint32 code : {0U, 4U, 8U, 999U, 1002U, 70000U, 0xFFFFFFFFU}) {
        EXPECT_EQ(decodeFromServer(code, {}).status, DecodeStatus::Unknown) << code;
        EXPECT_FALSE(isKnownServerCode(code));
    }
}

TEST(ServerMessages, UnknownConnectionTypeIsMalformed)
{
    Writer w;
    w.string(u"bob"_s);
    w.string(QByteArrayView("X"));
    w.u32(1);
    w.u32(2);
    w.u32(3);
    w.boolean(false);
    EXPECT_FALSE(fromserver::ConnectToPeerRequest::decode(w.data()).has_value());
}

// --- caps on server-controlled counts (protocol §12.7) ---------------------------------------------------------------

TEST(ServerMessages, RoomJoinedWithMismatchedParallelListsIsMalformed)
{
    fromserver::RoomJoined room;
    room.room = u"r"_s;
    room.members = {RoomMember{u"a"_s, UserStatus::Online, {}, 0, {}},
                    RoomMember{u"b"_s, UserStatus::Online, {}, 0, {}}};
    QByteArray payload = room.encode();
    // Patch the status list's count (right after the two names) from 2 to 3.
    const qsizetype statusCount = 4 + 1 + 4 + 4 + 1 + 4 + 1;
    ASSERT_EQ(payload.at(statusCount), 2);
    payload[statusCount] = 3;
    EXPECT_FALSE(fromserver::RoomJoined::decode(payload).has_value());
}

TEST(ServerMessages, RoomJoinedMemberCountMustFitTheBytes)
{
    Writer w;
    w.string(u"r"_s);
    w.u32(1000); // a thousand members announced, no data
    EXPECT_FALSE(fromserver::RoomJoined::decode(w.data()).has_value());
}

TEST(ServerMessages, RoomJoinedAboveTheMemberCapIsMalformed)
{
    // 50 001 members of 37 bytes each would fit in the payload but exceed the sanity cap.
    const qsizetype n = protocol::kRoomMembersMax + 1;
    Writer w(n * 40);
    w.string(u"big"_s);
    w.u32(static_cast<quint32>(n));
    for (qsizetype i = 0; i < n; ++i) {
        w.string(QByteArrayView("a"));
    }
    for (int list = 0; list < 4; ++list) {
        w.u32(static_cast<quint32>(n));
        for (qsizetype i = 0; i < n; ++i) {
            if (list == 1) {
                for (int s = 0; s < 5; ++s) {
                    w.u32(0);
                }
            } else if (list == 3) {
                w.string(QByteArrayView(""));
            } else {
                w.u32(0);
            }
        }
    }
    EXPECT_FALSE(fromserver::RoomJoined::decode(w.data()).has_value());
}

TEST(ServerMessages, RoomListWithMismatchedCountsIsMalformed)
{
    Writer w;
    w.u32(2);
    w.string(u"a"_s);
    w.string(u"b"_s);
    w.u32(1); // one count for two names
    w.u32(5);
    EXPECT_FALSE(fromserver::RoomList::decode(w.data()).has_value());
}

TEST(ServerMessages, PossibleParentsAboveTheCapIsMalformed)
{
    fromserver::PossibleParents parents;
    for (qsizetype i = 0; i <= protocol::kPossibleParentsMax; ++i) {
        parents.candidates.append(ParentCandidate{u"p"_s, 1, 2});
    }
    EXPECT_FALSE(fromserver::PossibleParents::decode(parents.encode()).has_value());
}

TEST(ServerMessages, EmbeddedMessageKeepsTheRawDistributedPayload)
{
    const QByteArray inner = QByteArray::fromHex("31 00 00 00 01 00 00 00 61 07 00 00 00 01 00 00 00 71");
    const QByteArray payload = QByteArray::fromHex("03") + inner;
    const std::optional<fromserver::EmbeddedMessage> embedded = fromserver::EmbeddedMessage::decode(payload);
    ASSERT_TRUE(embedded.has_value());
    EXPECT_EQ(embedded->distribCode, 3);
    EXPECT_EQ(embedded->payload, inner);
    EXPECT_FALSE(fromserver::EmbeddedMessage::decode({}).has_value());
}

TEST(ServerMessages, WatchUserReplyForAMissingUserHasNoStats)
{
    const QByteArray payload = QByteArray::fromHex("05 00 00 00 67 68 6f 73 74 00");
    const std::optional<fromserver::WatchUserReply> reply = fromserver::WatchUserReply::decode(payload);
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->exists);
    EXPECT_EQ(reply->stats, UserStats{});
}

TEST(ServerMessages, UnknownStatusValuesAreKept)
{
    Writer w;
    w.string(u"x"_s);
    w.u32(7); // not a status the protocol defines
    w.boolean(false);
    const std::optional<fromserver::UserStatusUpdate> update = fromserver::UserStatusUpdate::decode(w.data());
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(static_cast<quint32>(update->status), 7U);
}
