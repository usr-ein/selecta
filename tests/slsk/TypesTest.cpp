// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Types.h"

#include "slsk/Codec.h"
#include "slsk/TokenGenerator.h"

#include <QSet>

#include <array>
#include <gtest/gtest.h>
#include <limits>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;

TEST(RemotePath, DisplaysUtf8AndLatin1)
{
    EXPECT_EQ((RemotePath{QByteArray("Music\\Bj\xc3\xb6rk\\01.flac")}).display(), u"Music\\Björk\\01.flac"_s);
    EXPECT_EQ((RemotePath{QByteArray("Music\\Bj\xf6rk\\01.flac")}).display(), u"Music\\Björk\\01.flac"_s);
}

TEST(RemotePath, KeepsRawBytesThroughCopies)
{
    const RemotePath latin1{QByteArray("Bj\xf6rk")};
    RemotePath copy;
    copy = latin1;
    EXPECT_EQ(copy.raw, QByteArray("Bj\xf6rk")); // not re-encoded as UTF-8
    EXPECT_EQ(copy, latin1);
    EXPECT_NE(copy, RemotePath::fromText(u"Björk"_s));
}

TEST(RemotePath, Latin1VariantForTheFileNotSharedRetry)
{
    const RemotePath utf8 = RemotePath::fromText(u"Björk\\Début.mp3"_s);
    const std::optional<RemotePath> variant = utf8.latin1Variant();
    ASSERT_TRUE(variant.has_value());
    EXPECT_EQ(variant->raw, QByteArray("Bj\xf6rk\\D\xe9"
                                       "but.mp3"));
    // Plain ASCII encodes the same in both: no retry needed.
    EXPECT_FALSE(RemotePath::fromText(u"Artist\\Track.mp3"_s).latin1Variant().has_value());
    // Characters outside Latin-1 can't be retried.
    EXPECT_FALSE(RemotePath::fromText(u"日本\\曲.mp3"_s).latin1Variant().has_value());
    // A path that already isn't UTF-8 has no variant.
    EXPECT_FALSE((RemotePath{QByteArray("Bj\xf6rk")}).latin1Variant().has_value());
}

TEST(RemotePath, HashesByRawBytes)
{
    QSet<RemotePath> set;
    set.insert(RemotePath::fromText(u"a\\b"_s));
    set.insert(RemotePath::fromText(u"a\\b"_s));
    set.insert(RemotePath{QByteArray("a\\c")});
    EXPECT_EQ(set.size(), 2);
}

TEST(ConnectionTypes, ParseOnlyExactCodes)
{
    EXPECT_EQ(parseConnectionType("P"), ConnectionType::Peer);
    EXPECT_EQ(parseConnectionType("F"), ConnectionType::File);
    EXPECT_EQ(parseConnectionType("D"), ConnectionType::Distributed);
    EXPECT_FALSE(parseConnectionType("p").has_value());
    EXPECT_FALSE(parseConnectionType("PF").has_value());
    EXPECT_FALSE(parseConnectionType("").has_value());
    for (const ConnectionType type : {ConnectionType::Peer, ConnectionType::File, ConnectionType::Distributed}) {
        EXPECT_EQ(parseConnectionType(connectionTypeCode(type)), type);
    }
}

TEST(FileEntry, MissingVbrAttributeIsUnknownNotCbr)
{
    // A SoulseekQt MP3: bitrate and duration only.
    Writer w;
    w.u8(1);
    w.string(QByteArrayView("Music\\a.mp3"));
    w.u64(5'000'000);
    w.string(QByteArrayView(""));
    w.u32(2);
    w.u32(0);
    w.u32(320);
    w.u32(1);
    w.u32(215);
    Reader r(w.data());
    const std::optional<FileEntry> entry = FileEntry::decode(r);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->attributes.bitrate, 320U);
    EXPECT_EQ(entry->attributes.duration, 215U);
    EXPECT_FALSE(entry->attributes.vbr.has_value());
    EXPECT_FALSE(entry->attributes.sampleRate.has_value());
    EXPECT_TRUE(entry->extension.isEmpty());
}

TEST(FileEntry, DecodesEveryAttributeFaithfully)
{
    Writer w;
    w.u8(1);
    w.string(QByteArrayView("a.wv"));
    w.u64(1);
    w.string(QByteArrayView("wv"));
    w.u32(7);
    const std::array<std::pair<quint32, quint32>, 7> pairs = {
        {{4, 44100}, {5, 24}, {0, 900}, {1, 300}, {2, 0}, {3, 17}, {99, 5}}};
    for (const auto &[code, value] : pairs) {
        w.u32(code);
        w.u32(value);
    }
    Reader r(w.data());
    const std::optional<FileEntry> entry = FileEntry::decode(r);
    ASSERT_TRUE(entry.has_value());
    const FileAttributes &a = entry->attributes;
    EXPECT_EQ(a.sampleRate, 44100U);
    EXPECT_EQ(a.bitDepth, 24U);
    EXPECT_EQ(a.bitrate, 900U);
    EXPECT_EQ(a.duration, 300U);
    EXPECT_EQ(a.vbr, false);      // an explicit 0 is CBR
    ASSERT_EQ(a.other.size(), 2); // encoder (obsolete) and an unknown code, kept in order
    EXPECT_EQ(a.other.at(0), std::make_pair(quint32{3}, quint32{17}));
    EXPECT_EQ(a.other.at(1), std::make_pair(quint32{99}, quint32{5}));
    EXPECT_EQ(entry->extension, u"wv"_s);
}

TEST(FileEntry, RepeatedAttributeKeepsTheFirstAndRecordsTheRest)
{
    Writer w;
    w.u8(1);
    w.string(QByteArrayView("x"));
    w.u64(1);
    w.string(QByteArrayView(""));
    w.u32(3);
    w.u32(0);
    w.u32(128);
    w.u32(0);
    w.u32(320);
    w.u32(2);
    w.u32(5); // any non-zero VBR value is true
    Reader r(w.data());
    const std::optional<FileEntry> entry = FileEntry::decode(r);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->attributes.bitrate, 128U);
    EXPECT_EQ(entry->attributes.vbr, true);
    ASSERT_EQ(entry->attributes.other.size(), 1);
    EXPECT_EQ(entry->attributes.other.at(0).second, 320U);
}

TEST(FileEntry, AttributeCountLargerThanTheDataFails)
{
    Writer w;
    w.u8(1);
    w.string(QByteArrayView("x"));
    w.u64(1);
    w.string(QByteArrayView(""));
    w.u32(1'000'000); // a million pairs announced, none present
    Reader r(w.data());
    EXPECT_FALSE(FileEntry::decode(r).has_value());
}

TEST(FileEntry, RoundTrips)
{
    FileEntry entry;
    entry.name = RemotePath{QByteArray("Music\\Bj\xf6rk\\t.flac")};
    entry.size = 123'456'789'012ULL;
    entry.extension = u"flac"_s;
    entry.attributes.duration = 241;
    entry.attributes.sampleRate = 96000;
    entry.attributes.bitDepth = 24;
    entry.attributes.vbr = true;
    entry.attributes.other.append({42, 7});
    Writer w;
    entry.encode(w);
    Reader r(w.data());
    EXPECT_EQ(FileEntry::decode(r), entry);
    EXPECT_TRUE(r.atEnd());
}

TEST(FileSize, NegativeNsSizesAreReadAsUnsigned32Bit)
{
    EXPECT_EQ(normalizeFileSize(0xFFFF'FFFF'8000'0000ULL), 0x8000'0000ULL); // 2 GiB sent as int32 -2^31
    EXPECT_EQ(normalizeFileSize(0xFFFF'FFFF'FFFF'FFFFULL), 0xFFFF'FFFFULL);
    EXPECT_EQ(normalizeFileSize(5'000'000'000ULL), 5'000'000'000ULL); // a real 64-bit size is kept
    EXPECT_EQ(normalizeFileSize(0), 0ULL);
}

TEST(Rejection, ClassifiesTheExactStrings)
{
    struct Case
    {
        const char16_t *text = nullptr;
        RejectionReason reason = RejectionReason::Other;
        RejectionClass rejectionClass = RejectionClass::Never;
    };
    const std::array cases = {
        Case{u"Banned", RejectionReason::Banned, RejectionClass::Banned},
        Case{u"Banned (Banned, don't bother retrying)", RejectionReason::Banned, RejectionClass::Banned},
        Case{u"Cancelled", RejectionReason::Cancelled, RejectionClass::Never},
        Case{u"Complete", RejectionReason::Complete, RejectionClass::Never},
        Case{u"File not shared.", RejectionReason::FileNotShared, RejectionClass::Never},
        Case{u"File not shared", RejectionReason::FileNotShared, RejectionClass::Never},
        Case{u"File read error.", RejectionReason::FileReadError, RejectionClass::Retryable},
        Case{u"Pending shutdown.", RejectionReason::PendingShutdown, RejectionClass::Retryable},
        Case{u"Queued", RejectionReason::Queued, RejectionClass::Retryable},
        Case{u"Too many files", RejectionReason::TooManyFiles, RejectionClass::Retryable},
        Case{u"Too many megabytes", RejectionReason::TooManyMegabytes, RejectionClass::Retryable},
        Case{u"User limit of 100 files exceeded", RejectionReason::TooManyFiles, RejectionClass::Retryable},
        Case{u"User limit of 500 megabytes exceeded", RejectionReason::TooManyMegabytes, RejectionClass::Retryable},
        Case{u"Blocked country", RejectionReason::BlockedCountry, RejectionClass::Never},
        Case{u"Disallowed extension", RejectionReason::DisallowedExtension, RejectionClass::Never},
        Case{u"Remote file error", RejectionReason::RemoteFileError, RejectionClass::Never},
        // Case and the trailing dot matter; anything else is a generic denial.
        Case{u"banned", RejectionReason::Other, RejectionClass::Never},
        Case{u"file not shared.", RejectionReason::Other, RejectionClass::Never},
        Case{u"Banned!", RejectionReason::Other, RejectionClass::Never},
        Case{u"Enqueue failed due to internal error", RejectionReason::Other, RejectionClass::Never},
        Case{u"", RejectionReason::Other, RejectionClass::Never},
    };
    for (const auto &c : cases) {
        const QString text = QString::fromUtf16(c.text);
        const Rejection rejection = classifyRejection(text);
        EXPECT_EQ(rejection.text, text);
        EXPECT_EQ(rejection.reason, c.reason) << text.toStdString();
        EXPECT_EQ(rejection.rejectionClass, c.rejectionClass) << text.toStdString();
    }
}

TEST(Rejection, TextsInUseRoundTrip)
{
    for (const RejectionReason reason :
         {RejectionReason::Banned, RejectionReason::Cancelled, RejectionReason::Complete,
          RejectionReason::FileNotShared, RejectionReason::FileReadError, RejectionReason::PendingShutdown,
          RejectionReason::Queued, RejectionReason::TooManyFiles, RejectionReason::TooManyMegabytes}) {
        EXPECT_EQ(classifyRejection(rejectionText(reason)).reason, reason);
    }
    EXPECT_EQ(rejectionText(RejectionReason::FileNotShared), u"File not shared."_s);
    EXPECT_TRUE(rejectionText(RejectionReason::BlockedCountry).isEmpty()); // deprecated: never sent
    EXPECT_TRUE(rejectionText(RejectionReason::Other).isEmpty());
}

TEST(TokenGenerator, IncrementsFromItsStart)
{
    TokenGenerator tokens(41);
    EXPECT_EQ(tokens.next(), 41U);
    EXPECT_EQ(tokens.next(), 42U);
}

TEST(TokenGenerator, WrapsAndSkipsZero)
{
    TokenGenerator tokens(std::numeric_limits<quint32>::max());
    EXPECT_EQ(tokens.next(), std::numeric_limits<quint32>::max());
    EXPECT_EQ(tokens.next(), 1U);
    TokenGenerator fromZero(0);
    EXPECT_EQ(fromZero.next(), 1U);
}

TEST(TokenGenerator, SkipsTokensInUse)
{
    TokenGenerator tokens(10);
    const QSet<quint32> inUse{10, 11, 13};
    EXPECT_EQ(tokens.next([&](quint32 t) { return inUse.contains(t); }), 12U);
    EXPECT_EQ(tokens.next([&](quint32 t) { return inUse.contains(t); }), 14U);
}

TEST(TokenGenerator, GivesUpWhenEverythingProbedIsInUse)
{
    TokenGenerator tokens(1);
    EXPECT_EQ(tokens.next([](quint32) { return true; }), 0U);
}

TEST(TokenGenerator, RandomStartIsSmallAndNonZero)
{
    QSet<quint32> starts;
    for (int i = 0; i < 50; ++i) {
        const quint32 start = TokenGenerator::randomStart();
        EXPECT_GE(start, 1U);
        EXPECT_LE(start, (std::numeric_limits<quint32>::max() / 1000) + 1);
        starts.insert(start);
    }
    EXPECT_GT(starts.size(), 40); // random, not a constant
}
