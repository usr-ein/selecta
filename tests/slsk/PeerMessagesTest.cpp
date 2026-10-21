// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/PeerMessages.h"

#include "fixtures/Samples.h"
#include "slsk/Frame.h"
#include "slsk/Zlib.h"

#include <QRandomGenerator>
#include <QSet>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using selecta::slsk::testing::codeOf;
using selecta::slsk::testing::payloadOf;
using selecta::slsk::testing::peerInitSamples;
using selecta::slsk::testing::peerSamples;
using selecta::slsk::testing::sampleShareList;

namespace {

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
                const auto dispatched = decodeByCode(static_cast<quint32>(M::kCode), payload);
                ASSERT_EQ(dispatched.status, DecodeStatus::Decoded);
                ASSERT_TRUE(std::holds_alternative<M>(*dispatched.message));
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

void writeEntry(Writer &w, QByteArrayView name, quint64 size, QByteArrayView extension,
                std::initializer_list<std::pair<quint32, quint32>> attributes)
{
    w.u8(1);
    w.string(name);
    w.u64(size);
    w.string(extension);
    w.u32(static_cast<quint32>(attributes.size()));
    for (const auto &[code, value] : attributes) {
        w.u32(code);
        w.u32(value);
    }
}

} // namespace

// --- golden vectors --------------------------------------------------------------------------------------------------

TEST(PeerMessages, GoldenPeerInitAndPierceFireWall)
{
    EXPECT_EQ(frame(peerinit::PeerInit{u"alice"_s, ConnectionType::Peer, 0}).toHex(' '),
              "13 00 00 00 01 05 00 00 00 61 6c 69 63 65 01 00 00 00 50 00 00 00 00");
    EXPECT_EQ(frame(peerinit::PierceFireWall{0x01020304}).toHex(' '), "05 00 00 00 00 04 03 02 01");
}

TEST(PeerMessages, PeerInitTokenIsReadAndIgnored)
{
    // aioslsk sends a 64-bit token; very old clients none at all.
    Writer w;
    w.string(u"a"_s);
    w.string(QByteArrayView("P"));
    const QByteArray noToken = w.data();
    w.u64(0x1122334455667788ULL);
    const QByteArray longToken = w.data();
    ASSERT_TRUE(peerinit::PeerInit::decode(noToken).has_value());
    EXPECT_EQ(peerinit::PeerInit::decode(noToken)->token, 0U);
    ASSERT_TRUE(peerinit::PeerInit::decode(longToken).has_value());
    EXPECT_EQ(peerinit::PeerInit::decode(longToken)->type, ConnectionType::Peer);
}

TEST(PeerMessages, PeerInitWithAnUnknownTypeIsMalformed)
{
    Writer w;
    w.string(u"a"_s);
    w.string(QByteArrayView("X"));
    w.u32(0);
    EXPECT_FALSE(peerinit::PeerInit::decode(w.data()).has_value());
}

TEST(PeerMessages, GoldenTransferMessages)
{
    EXPECT_EQ(peer::TransferRequest({TransferDirection::Upload, 7, RemotePath::fromText(u"a\\b"_s), 1024})
                  .encode()
                  .toHex(' '),
              "01 00 00 00 07 00 00 00 03 00 00 00 61 5c 62 00 04 00 00 00 00 00 00");
    EXPECT_EQ(peer::TransferResponse({7, true, std::nullopt, {}}).encode().toHex(' '), "07 00 00 00 01");
    EXPECT_EQ(peer::TransferResponse({7, false, std::nullopt, u"Cancelled"_s}).encode().toHex(' '),
              "07 00 00 00 00 09 00 00 00 43 61 6e 63 65 6c 6c 65 64");
    EXPECT_EQ(file::encodeOffset(0x1'0000'0001ULL).toHex(' '), "01 00 00 00 01 00 00 00");
    EXPECT_EQ(file::encodeTransferInit(42).toHex(' '), "2a 00 00 00");
}

TEST(PeerMessages, LegacyDownloadRequestWithASizeStillDecodes)
{
    // Soulseek.NET writes the 64-bit size even for direction 0.
    const peer::TransferRequest request{TransferDirection::Download, 3, RemotePath::fromText(u"x"_s), 99};
    const std::optional<peer::TransferRequest> decoded = peer::TransferRequest::decode(request.encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->direction, TransferDirection::Download);
    EXPECT_EQ(decoded->size, 99U);
}

TEST(PeerMessages, TransferResponseRefusalWithoutReasonDecodes)
{
    const std::optional<peer::TransferResponse> response =
        peer::TransferResponse::decode(QByteArray::fromHex("07 00 00 00 00"));
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->allowed);
    EXPECT_TRUE(response->reason.isEmpty());
}

TEST(PeerMessages, FileTransferHandshakeNeedsExactSizes)
{
    EXPECT_EQ(file::decodeTransferInit(file::encodeTransferInit(0xCAFEBABE)), 0xCAFEBABEU);
    EXPECT_FALSE(file::decodeTransferInit(QByteArray(3, '\0')).has_value());
    EXPECT_FALSE(file::decodeTransferInit(QByteArray(5, '\0')).has_value());
    EXPECT_EQ(file::decodeOffset(file::encodeOffset(1234567890123ULL)), 1234567890123ULL);
    EXPECT_FALSE(file::decodeOffset(QByteArray(7, '\0')).has_value());
}

// --- zlib messages: layouts built by hand, independently of the encoders ---------------------------------------------

TEST(PeerMessages, DecodesAHandBuiltSearchResponse)
{
    Writer body;
    body.string(QByteArrayView("responder"));
    body.u32(0x1234);
    body.u32(2);
    writeEntry(body, "Music\\A\\01.mp3", 4'000'000, "mp3", {{0, 320}, {1, 200}, {2, 0}});
    writeEntry(body, "Music\\A\\02.flac", 30'000'000, "", {{1, 180}, {4, 44100}, {5, 16}});
    body.boolean(true);
    body.u32(123'456);
    body.u32(5);
    body.u32(0);
    body.u32(1);
    writeEntry(body, "Music\\Locked\\x.mp3", 1, "", {});
    const QByteArray payload = zlib::deflate(body.data());

    const std::optional<peer::FileSearchResponse> response = peer::FileSearchResponse::decode(payload);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->username, u"responder"_s);
    EXPECT_EQ(response->token, 0x1234U);
    ASSERT_EQ(response->results.size(), 2);
    EXPECT_EQ(response->results[0].name.raw, QByteArray("Music\\A\\01.mp3"));
    EXPECT_EQ(response->results[0].attributes.vbr, false);
    EXPECT_EQ(response->results[1].attributes.sampleRate, 44100U);
    EXPECT_FALSE(response->results[1].attributes.vbr.has_value());
    EXPECT_TRUE(response->slotFree);
    EXPECT_EQ(response->averageSpeed, 123'456U);
    EXPECT_EQ(response->queueLength, 5U);
    ASSERT_EQ(response->privateResults.size(), 1);

    const std::optional<peer::FileSearchResponse::Header> header = peer::FileSearchResponse::peek(payload);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->username, u"responder"_s);
    EXPECT_EQ(header->token, 0x1234U);
}

TEST(PeerMessages, SearchResponseWithoutTrailingFieldsDecodes)
{
    Writer body;
    body.string(QByteArrayView("old"));
    body.u32(9);
    body.u32(0);
    const std::optional<peer::FileSearchResponse> response =
        peer::FileSearchResponse::decode(zlib::deflate(body.data()));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->results.isEmpty());
    EXPECT_FALSE(response->slotFree);
}

TEST(PeerMessages, SearchResponseOverTheInflateCapIsRefused)
{
    peer::FileSearchResponse big;
    big.username = u"x"_s;
    for (int i = 0; i < 2000; ++i) {
        big.results.append(selecta::slsk::testing::sampleFile(i));
    }
    const QByteArray payload = big.encode();
    EXPECT_TRUE(peer::FileSearchResponse::decode(payload).has_value());
    EXPECT_FALSE(peer::FileSearchResponse::decode(payload, 10'000).has_value());
    // The header can still be peeked cheaply.
    EXPECT_TRUE(peer::FileSearchResponse::peek(payload).has_value());
}

TEST(PeerMessages, PeekRefusesAnOverlongUsername)
{
    Writer body;
    body.string(QByteArray(protocol::kUsernameMaxBytes + 1, 'u'));
    body.u32(1);
    EXPECT_FALSE(peer::FileSearchResponse::peek(zlib::deflate(body.data())).has_value());
}

TEST(PeerMessages, PeekRefusesGarbage)
{
    EXPECT_FALSE(peer::FileSearchResponse::peek("not zlib").has_value());
    EXPECT_FALSE(peer::FileSearchResponse::peek({}).has_value());
}

TEST(PeerMessages, DecodesAHandBuiltShareList)
{
    Writer body;
    body.u32(2);
    body.string(QByteArrayView("Collection\\Album"));
    body.u32(1);
    writeEntry(body, "01 Track.flac", 25'000'000, "", {{1, 300}, {4, 48000}, {5, 24}});
    body.string(QByteArrayView("Collection\\Empty"));
    body.u32(0);
    body.u32(0); // unknown
    body.u32(1);
    body.string(QByteArrayView("Collection\\Buddies only"));
    body.u32(0);
    const std::optional<peer::SharedFileListResponse> response =
        peer::SharedFileListResponse::decode(zlib::deflate(body.data()));
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->list.directories.size(), 2);
    EXPECT_EQ(response->list.directories[0].files[0].name.raw, QByteArray("01 Track.flac"));
    EXPECT_TRUE(response->list.directories[1].files.isEmpty());
    ASSERT_EQ(response->list.privateDirectories.size(), 1);
}

TEST(PeerMessages, ShareListWithoutPrivateSectionDecodes)
{
    Writer body;
    body.u32(0);
    EXPECT_TRUE(peer::SharedFileListResponse::decode(zlib::deflate(body.data())).has_value());
}

TEST(PeerMessages, LargeShareListRoundTripsWithinItsCap)
{
    const peer::SharedFileListResponse big{sampleShareList(200, 50)};
    const QByteArray payload = big.encode();
    const std::optional<peer::SharedFileListResponse> decoded = peer::SharedFileListResponse::decode(payload);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, big);
    EXPECT_FALSE(peer::SharedFileListResponse::decode(payload, 1024).has_value());
    // Why it failed: over the cap, not malformed.
    bool tooLarge = false;
    EXPECT_FALSE(peer::SharedFileListResponse::decode(payload, 1024, &tooLarge).has_value());
    EXPECT_TRUE(tooLarge);
    EXPECT_TRUE(peer::SharedFileListResponse::decode(payload, protocol::kShareListInflatedMax, &tooLarge).has_value());
    EXPECT_FALSE(tooLarge);
}

TEST(PeerMessages, MalformedShareListIsNotTooLarge)
{
    Writer body;
    body.u32(3); // three folders announced, none follows
    bool tooLarge = true;
    EXPECT_FALSE(
        peer::SharedFileListResponse::decode(zlib::deflate(body.data()), protocol::kShareListInflatedMax, &tooLarge)
            .has_value());
    EXPECT_FALSE(tooLarge);
    tooLarge = true;
    EXPECT_FALSE(
        peer::SharedFileListResponse::decode("not zlib", protocol::kShareListInflatedMax, &tooLarge).has_value());
    EXPECT_FALSE(tooLarge);
}

TEST(PeerMessages, ShareListWithTrailingGarbageAfterTheStreamIsRefused)
{
    QByteArray payload = peer::SharedFileListResponse{sampleShareList(1, 1)}.encode();
    payload.append("x");
    EXPECT_FALSE(peer::SharedFileListResponse::decode(payload).has_value());
}

TEST(PeerMessages, FolderContentsKeepsRawFolderBytes)
{
    const peer::FolderContentsResponse response{9, RemotePath{QByteArray("M\xfcsic")}, {}};
    const std::optional<peer::FolderContentsResponse> decoded = peer::FolderContentsResponse::decode(response.encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->folder.raw, QByteArray("M\xfcsic"));
}

TEST(PeerMessages, UserInfoToleratesAShortTail)
{
    // Museek+ sends slotsFree as a 32-bit value: a 3-byte tail that must not be read as uploadPermitted.
    Writer w;
    w.string(u"desc"_s);
    w.boolean(false);
    w.u32(2);
    w.u32(0);
    w.u32(1); // "slotsFree" as uint32
    const std::optional<peer::UserInfoResponse> info = peer::UserInfoResponse::decode(w.data());
    ASSERT_TRUE(info.has_value());
    EXPECT_TRUE(info->slotsFree);
    EXPECT_FALSE(info->uploadPermitted.has_value());
}

TEST(PeerMessages, UserInfoPictureLongerThanThePayloadIsMalformed)
{
    Writer w;
    w.string(u"d"_s);
    w.boolean(true);
    w.u32(1'000'000);
    w.bytes("tiny");
    EXPECT_FALSE(peer::UserInfoResponse::decode(w.data()).has_value());
}

// --- string caps (protocol::kStringMaxBytes) ---------------------------------------------------------------------

namespace {

const QByteArray kNameAtCap(protocol::kStringMaxBytes, 'n');
const QByteArray kNameOverCap(protocol::kStringMaxBytes + 1, 'n');

// A search response with one public and one private result, named as given.
QByteArray searchResponseNaming(QByteArrayView publicName, QByteArrayView privateName)
{
    Writer body;
    body.string(QByteArrayView("responder"));
    body.u32(1);
    body.u32(1);
    writeEntry(body, publicName, 1, "mp3", {});
    body.boolean(true);
    body.u32(0);
    body.u32(0);
    body.u32(0);
    body.u32(1);
    writeEntry(body, privateName, 1, "mp3", {});
    return zlib::deflate(body.data());
}

// A share list of `files` files in one folder, then one private folder; the names are as given.
QByteArray shareListNaming(QByteArrayView folder, QByteArrayView lastFile, QByteArrayView privateFolder,
                           int files = 1)
{
    Writer body;
    body.u32(1);
    body.string(folder);
    body.u32(static_cast<quint32>(files));
    for (int i = 0; i < files - 1; ++i) {
        writeEntry(body, "x.mp3", 1, "", {});
    }
    writeEntry(body, lastFile, 1, "", {});
    body.u32(0);
    body.u32(1);
    body.string(privateFolder);
    body.u32(0);
    return zlib::deflate(body.data());
}

} // namespace

TEST(PeerMessages, SearchResponseWithNamesAtTheCapDecodes)
{
    const std::optional<peer::FileSearchResponse> response =
        peer::FileSearchResponse::decode(searchResponseNaming(kNameAtCap, kNameAtCap));
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->results.size(), 1);
    EXPECT_EQ(response->results[0].name.raw, kNameAtCap);
    ASSERT_EQ(response->privateResults.size(), 1);
}

TEST(PeerMessages, SearchResponseWithAnOversizeNameIsMalformed)
{
    EXPECT_FALSE(peer::FileSearchResponse::decode(searchResponseNaming(kNameOverCap, "a.mp3")).has_value());
    // In the optional private section too: an oversize name isn't junk to skip, it fails the whole message.
    EXPECT_FALSE(peer::FileSearchResponse::decode(searchResponseNaming("a.mp3", kNameOverCap)).has_value());
    // Any string of the entry: its extension as well.
    Writer body;
    body.string(QByteArrayView("responder"));
    body.u32(1);
    body.u32(1);
    writeEntry(body, "a.mp3", 1, kNameOverCap, {});
    const QByteArray payload = zlib::deflate(body.data());
    EXPECT_FALSE(peer::FileSearchResponse::decode(payload).has_value());
    EXPECT_EQ(decodePeer(static_cast<quint32>(PeerCode::FileSearchResponse), payload).status, DecodeStatus::Malformed);
}

TEST(PeerMessages, ShareListWithNamesAtTheCapDecodes)
{
    const std::optional<peer::SharedFileListResponse> response =
        peer::SharedFileListResponse::decode(shareListNaming(kNameAtCap, kNameAtCap, kNameAtCap));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->list.directories[0].path.raw, kNameAtCap);
    EXPECT_EQ(response->list.directories[0].files[0].name.raw, kNameAtCap);
    EXPECT_EQ(response->list.privateDirectories[0].path.raw, kNameAtCap);
}

TEST(PeerMessages, ShareListWithAnOversizeNameIsMalformedNotTooLarge)
{
    // The case P-005's review found: one huge folder path over many files, which a reader would copy into each.
    for (const QByteArray &payload : {shareListNaming(kNameOverCap, "a.mp3", "p", 100'000),
                                      shareListNaming("Music", kNameOverCap, "p", 100'000),
                                      shareListNaming("Music", "a.mp3", kNameOverCap)}) {
        bool tooLarge = true;
        EXPECT_FALSE(
            peer::SharedFileListResponse::decode(payload, protocol::kShareListInflatedMax, &tooLarge).has_value());
        EXPECT_FALSE(tooLarge);
    }
}

TEST(PeerMessages, FolderAndTransferMessagesWithAnOversizePathAreMalformed)
{
    const RemotePath oversize{kNameOverCap};
    const QList<PeerMessage> messages{
        peer::FolderContentsRequest{1, oversize},
        peer::FolderContentsResponse{1, oversize, {}},
        peer::TransferRequest{TransferDirection::Download, 1, oversize, std::nullopt},
        peer::QueueUpload{oversize},
        peer::UploadFailed{oversize},
        peer::PlaceInQueueRequest{oversize},
        peer::PlaceInQueueResponse{oversize, 1},
        peer::UploadDenied{oversize, u"Banned"_s},
    };
    for (const PeerMessage &message : messages) {
        EXPECT_EQ(decodePeer(codeOf(message), payloadOf(message)).status, DecodeStatus::Malformed)
            << "code " << codeOf(message);
    }
}

TEST(PeerMessages, UserInfoPictureMayExceedTheStringCap)
{
    peer::UserInfoResponse info;
    info.description = QString(protocol::kStringMaxBytes, u'd');
    info.picture = QByteArray(protocol::kStringMaxBytes * 4, 'p');
    const std::optional<peer::UserInfoResponse> decoded = peer::UserInfoResponse::decode(info.encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, info);

    info.description.append(u'd');
    EXPECT_FALSE(peer::UserInfoResponse::decode(info.encode()).has_value());
}

TEST(PeerMessages, GetShareFileListAcceptsAnOptionalTicket)
{
    EXPECT_TRUE(peer::GetShareFileList::decode({}).has_value());
    EXPECT_TRUE(peer::GetShareFileList::decode(QByteArray::fromHex("01 00 00 00")).has_value());
}

// --- round trips, truncation, garbage --------------------------------------------------------------------------------

TEST(PeerMessages, SamplesCoverEveryMessage)
{
    QSet<std::size_t> covered;
    const QList<PeerMessage> allPeerSamples = peerSamples();
    for (const PeerMessage &sample : allPeerSamples) {
        covered.insert(sample.index());
    }
    EXPECT_EQ(static_cast<std::size_t>(covered.size()), std::variant_size_v<PeerMessage>);
    QSet<std::size_t> coveredInit;
    const QList<PeerInitMessage> allPeerInitSamples = peerInitSamples();
    for (const PeerInitMessage &sample : allPeerInitSamples) {
        coveredInit.insert(sample.index());
    }
    EXPECT_EQ(static_cast<std::size_t>(coveredInit.size()), std::variant_size_v<PeerInitMessage>);
}

TEST(PeerMessages, EveryMessageRoundTrips)
{
    expectRoundTrips(peerSamples(), decodePeer);
    expectRoundTrips(peerInitSamples(), decodePeerInit);
}

TEST(PeerMessages, TruncatedPayloadsNeverCrashAndZlibOnesAreMalformed)
{
    const QList<PeerMessage> allPeerSamples = peerSamples();
    for (const PeerMessage &sample : allPeerSamples) {
        const QByteArray payload = payloadOf(sample);
        const bool compressed = std::holds_alternative<peer::SharedFileListResponse>(sample) ||
                                std::holds_alternative<peer::FileSearchResponse>(sample) ||
                                std::holds_alternative<peer::FolderContentsResponse>(sample);
        for (qsizetype cut = 0; cut < payload.size(); ++cut) {
            const Decoded<PeerMessage> decoded = decodePeer(codeOf(sample), payload.left(cut));
            if (compressed) {
                EXPECT_EQ(decoded.status, DecodeStatus::Malformed) << codeOf(sample) << " cut " << cut;
            }
        }
    }
}

TEST(PeerMessages, RandomPayloadsNeverCrash)
{
    QRandomGenerator random(99);
    for (int i = 0; i < 3000; ++i) {
        const quint32 code = random.bounded(60U);
        const QByteArray payload = randomBytes(random, random.bounded(80));
        (void)decodePeer(code, payload);
        (void)decodePeerInit(code % 3, payload);
        (void)peer::FileSearchResponse::peek(payload);
    }
    // Random bytes inside a valid zlib stream exercise the post-inflate parsers.
    for (int i = 0; i < 1000; ++i) {
        const QByteArray body = zlib::deflate(randomBytes(random, random.bounded(200)));
        (void)peer::SharedFileListResponse::decode(body);
        (void)peer::FileSearchResponse::decode(body);
        (void)peer::FolderContentsResponse::decode(body);
    }
    SUCCEED();
}

TEST(PeerMessages, KnownAndUnknownCodes)
{
    for (const quint32 code : {1U, 8U, 10U, 14U, 33U, 34U, 42U, 47U, 48U, 49U, 52U}) {
        EXPECT_EQ(decodePeer(code, {}).status, DecodeStatus::Ignored) << code;
    }
    for (const quint32 code : {0U, 2U, 3U, 6U, 53U, 1000U}) {
        EXPECT_EQ(decodePeer(code, {}).status, DecodeStatus::Unknown) << code;
    }
    EXPECT_EQ(decodePeerInit(2, {}).status, DecodeStatus::Unknown);
}

TEST(PeerMessages, AcceptablePeerUsernames)
{
    EXPECT_TRUE(isAcceptablePeerUsername(u"alice"_s));
    EXPECT_TRUE(isAcceptablePeerUsername(u"Björk 🎵"_s));
    EXPECT_TRUE(isAcceptablePeerUsername(u"with space"_s));
    EXPECT_FALSE(isAcceptablePeerUsername(u""_s));
    EXPECT_FALSE(isAcceptablePeerUsername(u"server"_s));
    EXPECT_FALSE(isAcceptablePeerUsername(u"bad\nname"_s));
    EXPECT_FALSE(isAcceptablePeerUsername(u"bad\u0007bell"_s));
    EXPECT_FALSE(isAcceptablePeerUsername(QString(protocol::kUsernameMaxBytes + 1, u'a')));
    EXPECT_TRUE(isAcceptablePeerUsername(QString(protocol::kUsernameMaxBytes, u'a')));
}
