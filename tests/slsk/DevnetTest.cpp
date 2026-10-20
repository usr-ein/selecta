// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-devnet's parts (tools/devnet/): header probing, library scanning, search matching, message descriptions,
// the corpus plan, and the network end to end against slsk's own session, peer and transfer code.
#include "AudioProbe.h"
#include "Behaviour.h"
#include "Corpus.h"
#include "Describe.h"
#include "Library.h"
#include "Network.h"
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"
#include "slsk/Frame.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"
#include "slsk/TransferManager.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <gtest/gtest.h>
#include <map>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using namespace selecta::devnet;
using selecta::governor::Operation;
using selecta::slsk::testing::permit;
using selecta::slsk::testing::settle;
using selecta::slsk::testing::TestPermits;
using selecta::slsk::testing::waitFor;

namespace slsk = selecta::slsk;

namespace {

// --- synthetic headers -----------------------------------------------------------------------------------------------

void appendBe32(QByteArray &bytes, quint32 value)
{
    const quint32 be = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&be), 4); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

void appendLe32(QByteArray &bytes, quint32 value)
{
    const quint32 le = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&le), 4); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

void appendLe16(QByteArray &bytes, quint16 value)
{
    const quint16 le = qToLittleEndian(value);
    bytes.append(reinterpret_cast<const char *>(&le), 2); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

void appendBe16(QByteArray &bytes, quint16 value)
{
    const quint16 be = qToBigEndian(value);
    bytes.append(reinterpret_cast<const char *>(&be), 2); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

// MPEG-1 Layer III frames at 320 kbps, 44.1 kHz, stereo: 1044 bytes each. The first can carry a Xing/Info or VBRI
// header (`tag`, at the offset each one lives at) with a frame count and a byte count.
QByteArray mp3Frames(int count, const char *tag = nullptr, quint32 frames = 0, quint32 bytes = 0)
{
    constexpr qsizetype kFrame = 1044;
    QByteArray frame(kFrame, '\0');
    frame[0] = static_cast<char>(0xFF);
    frame[1] = static_cast<char>(0xFB);
    frame[2] = static_cast<char>(0xE0);
    QByteArray first = frame;
    if (tag != nullptr) {
        const bool vbri = qstrcmp(tag, "VBRI") == 0;
        QByteArray header(tag, 4);
        if (vbri) {
            appendBe16(header, 1); // version
            appendBe16(header, 0); // delay
            appendBe16(header, 0); // quality
            appendBe32(header, bytes);
            appendBe32(header, frames);
        } else {
            appendBe32(header, 3); // frames and bytes present
            appendBe32(header, frames);
            appendBe32(header, bytes);
        }
        first.replace(4 + 32, header.size(), header);
    }
    QByteArray out = first;
    for (int i = 1; i < count; ++i) {
        out += frame;
    }
    return out;
}

QByteArray flacHeader(quint32 sampleRate, quint32 bits, quint64 samples)
{
    QByteArray bytes("fLaC");
    bytes.append(static_cast<char>(0x80)); // last metadata block, type 0 (STREAMINFO)
    bytes.append(QByteArray::fromHex("000022"));
    bytes.append(QByteArray(10, '\0')); // block and frame sizes
    const quint64 packed = (quint64{sampleRate} << 44U) | (quint64{1} << 41U) | (quint64{bits - 1} << 36U) | samples;
    const quint64 be = qToBigEndian(packed);
    bytes.append(reinterpret_cast<const char *>(&be), 8); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    bytes.append(QByteArray(16, '\0'));                   // MD5
    return bytes;
}

QByteArray wavHeader(quint32 sampleRate, quint16 bits, quint32 dataBytes)
{
    QByteArray bytes("RIFF");
    appendLe32(bytes, 36 + dataBytes);
    bytes.append("WAVE");
    bytes.append("fmt ");
    appendLe32(bytes, 16);
    appendLe16(bytes, 1); // PCM
    appendLe16(bytes, 2);
    appendLe32(bytes, sampleRate);
    appendLe32(bytes, sampleRate * 2 * (bits / 8));
    appendLe16(bytes, static_cast<quint16>(2 * (bits / 8)));
    appendLe16(bytes, bits);
    bytes.append("LIST");
    appendLe32(bytes, 3); // odd: padded to 4
    bytes.append("abc");
    bytes.append('\0');
    bytes.append("data");
    appendLe32(bytes, dataBytes);
    return bytes;
}

QByteArray aiffHeader(quint32 frames, quint16 bits)
{
    QByteArray bytes("FORM");
    appendBe32(bytes, 0);
    bytes.append("AIFF");
    bytes.append("COMM");
    appendBe32(bytes, 18);
    appendBe16(bytes, 2);
    appendBe32(bytes, frames);
    appendBe16(bytes, bits);
    appendBe16(bytes, 0x400E); // 44100 as an 80-bit extended float
    appendBe32(bytes, 0xAC440000);
    appendBe32(bytes, 0);
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

slsk::SharedFileList sampleShares()
{
    const auto entry = [](const char *name) {
        return slsk::FileEntry{slsk::RemotePath::fromText(QString::fromUtf8(name)), 1, {}, {}};
    };
    slsk::SharedFileList list;
    list.directories.append(
        {slsk::RemotePath::fromText(u"lossless_lena\\Kel Varo\\Kel Varo - Paper Tide (2011) [FLAC]"_s),
         {entry("01 - Amber Engine.flac"), entry("02 - Slow Comet.flac"), entry("cover.jpg")}});
    list.directories.append({slsk::RemotePath::fromText(u"lossless_lena\\Mira Sønder\\Live at the Harbour"_s),
                             {entry("01 - Glass Signal.flac"), entry("02 - Kel's Return.flac")}});
    return list;
}

} // namespace

// --- AudioProbe ------------------------------------------------------------------------------------------------------

TEST(DevnetProbe, Id3v2SizeIsSyncsafeAndCountsTheFooter)
{
    EXPECT_EQ(id3v2Size(QByteArray::fromHex("49443303000000000201")), 10U + 257U);
    EXPECT_EQ(id3v2Size(QByteArray::fromHex("49443304001000000201")), 10U + 257U + 10U); // footer flag
    EXPECT_EQ(id3v2Size(QByteArray::fromHex("49443303000000008001")), 0U);               // not syncsafe
    EXPECT_EQ(id3v2Size(QByteArray("ID3")), 0U);
    EXPECT_EQ(id3v2Size(QByteArray("RIFF....WAVE")), 0U);
}

TEST(DevnetProbe, ConstantBitrateMp3)
{
    // Without a header: the duration comes from the size.
    const QByteArray plain = mp3Frames(100);
    const slsk::FileAttributes a = probeMp3(plain, static_cast<quint64>(plain.size()));
    EXPECT_EQ(a.bitrate, 320U);
    EXPECT_EQ(a.vbr, false);
    EXPECT_EQ(a.duration, 3U); // 104 400 bytes at 320 kbps: 2.6 s
    EXPECT_FALSE(a.sampleRate.has_value());
    // LAME's Info header: CBR with an exact frame count.
    const QByteArray info = mp3Frames(10, "Info", 4000, 4'176'000);
    const slsk::FileAttributes b = probeMp3(info, 4'176'000);
    EXPECT_EQ(b.bitrate, 320U);
    EXPECT_EQ(b.vbr, false);
    EXPECT_EQ(b.duration, 104U); // 4000 × 1152 / 44100
}

TEST(DevnetProbe, VariableBitrateMp3)
{
    const QByteArray xing = mp3Frames(4, "Xing", 1000, 1'000'000);
    const slsk::FileAttributes a = probeMp3(xing, 1'000'000);
    EXPECT_EQ(a.vbr, true);
    EXPECT_EQ(a.duration, 26U); // 1000 × 1152 / 44100 = 26.1 s
    EXPECT_EQ(a.bitrate, 306U); // the average: 8 Mbit over 26.1 s
    const QByteArray vbri = mp3Frames(4, "VBRI", 500, 500'000);
    const slsk::FileAttributes b = probeMp3(vbri, 500'000);
    EXPECT_EQ(b.vbr, true);
    EXPECT_EQ(b.duration, 13U);
    EXPECT_EQ(b.bitrate, 306U);
}

TEST(DevnetProbe, Mp3FindsTheFirstRealFrame)
{
    // Leftover bytes with a lone sync pattern before the audio.
    QByteArray bytes = QByteArray::fromHex("00FFFB00112233");
    bytes += mp3Frames(20);
    EXPECT_EQ(probeMp3(bytes, static_cast<quint64>(bytes.size())).bitrate, 320U);
    // MPEG-2 Layer III, 64 kbps, 22.05 kHz.
    QByteArray frame(208, '\0');
    frame[0] = static_cast<char>(0xFF);
    frame[1] = static_cast<char>(0xF3);
    frame[2] = static_cast<char>(0x80);
    QByteArray mpeg2;
    for (int i = 0; i < 50; ++i) {
        mpeg2 += frame;
    }
    const slsk::FileAttributes a = probeMp3(mpeg2, static_cast<quint64>(mpeg2.size()));
    EXPECT_EQ(a.bitrate, 64U);
    EXPECT_EQ(a.duration, 1U); // 10 400 bytes at 64 kbps
    // No frame at all.
    EXPECT_TRUE(probeMp3(QByteArray(4096, 'x'), 4096).isEmpty());
    EXPECT_TRUE(probeMp3(QByteArray(), 0).isEmpty());
}

TEST(DevnetProbe, Flac)
{
    const slsk::FileAttributes cd = probeFlac(flacHeader(44100, 16, 44100ULL * 30));
    EXPECT_EQ(cd.sampleRate, 44100U);
    EXPECT_EQ(cd.bitDepth, 16U);
    EXPECT_EQ(cd.duration, 30U);
    EXPECT_FALSE(cd.bitrate.has_value()); // lossless: no bitrate, as Nicotine+ sends it
    const slsk::FileAttributes hiRes = probeFlac(flacHeader(96000, 24, 96000ULL * 7));
    EXPECT_EQ(hiRes.sampleRate, 96000U);
    EXPECT_EQ(hiRes.bitDepth, 24U);
    EXPECT_EQ(hiRes.duration, 7U);
    QByteArray notStreamInfo = flacHeader(44100, 16, 1);
    notStreamInfo[4] = static_cast<char>(0x84); // a VORBIS_COMMENT block first
    EXPECT_TRUE(probeFlac(notStreamInfo).isEmpty());
    EXPECT_TRUE(probeFlac(flacHeader(0, 16, 1)).isEmpty());
    EXPECT_TRUE(probeFlac(QByteArray("fLaC")).isEmpty());
}

TEST(DevnetProbe, WavAndAiff)
{
    const QByteArray wav = wavHeader(44100, 16, 44100 * 4 * 5);
    const slsk::FileAttributes a = probeWav(wav, static_cast<quint64>(wav.size()) + (44100ULL * 4 * 5));
    EXPECT_EQ(a.sampleRate, 44100U);
    EXPECT_EQ(a.bitDepth, 16U);
    EXPECT_EQ(a.duration, 5U);
    // A data chunk claiming more than the file holds is cut to the file.
    EXPECT_EQ(probeWav(wav, static_cast<quint64>(wav.size()) + (44100ULL * 4 * 2)).duration, 2U);
    EXPECT_TRUE(probeWav(QByteArray("RIFF\0\0\0\0AVI ", 12), 12).isEmpty());

    const slsk::FileAttributes b = probeAiff(aiffHeader(44100 * 4, 16));
    EXPECT_EQ(b.sampleRate, 44100U);
    EXPECT_EQ(b.bitDepth, 16U);
    EXPECT_EQ(b.duration, 4U);
    EXPECT_TRUE(probeAiff(QByteArray("FORM\0\0\0\0AIFF", 12)).isEmpty());
}

TEST(DevnetProbe, FilesOnDisk)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // An MP3 behind an ID3v2 tag, with an ID3v1 tag at the end.
    QByteArray mp3 = QByteArray::fromHex("49443303000000000064") + QByteArray(100, '\0') + mp3Frames(100);
    mp3 += QByteArray("TAG") + QByteArray(125, ' ');
    ASSERT_TRUE(writeFile(dir.filePath(u"a.mp3"_s), mp3));
    EXPECT_EQ(probeAudio(dir.filePath(u"a.mp3"_s)).bitrate, 320U);
    EXPECT_EQ(probeAudio(dir.filePath(u"a.mp3"_s)).duration, 3U);
    ASSERT_TRUE(writeFile(dir.filePath(u"b.flac"_s), flacHeader(48000, 24, 48000ULL * 9)));
    EXPECT_EQ(probeAudio(dir.filePath(u"b.flac"_s)).duration, 9U);
    // Not audio, or not understood: no attributes.
    ASSERT_TRUE(writeFile(dir.filePath(u"cover.jpg"_s), QByteArray::fromHex("FFD8FFE0") + QByteArray(100, 'j')));
    EXPECT_TRUE(probeAudio(dir.filePath(u"cover.jpg"_s)).isEmpty());
    EXPECT_TRUE(probeAudio(dir.filePath(u"missing.mp3"_s)).isEmpty());
}

// --- Library ---------------------------------------------------------------------------------------------------------

TEST(DevnetLibrary, EachSubfolderIsAPeer)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.path();
    ASSERT_TRUE(writeFile(root + u"/alice/Kel Varo/Album/02 - Two.mp3"_s, mp3Frames(50)));
    ASSERT_TRUE(writeFile(root + u"/alice/Kel Varo/Album/01 - One.flac"_s, flacHeader(44100, 16, 44100ULL * 3)));
    ASSERT_TRUE(writeFile(root + u"/alice/Kel Varo/Album/cover.jpg"_s, QByteArray(10, 'c')));
    ASSERT_TRUE(writeFile(root + u"/alice/loose.mp3"_s, mp3Frames(5)));
    ASSERT_TRUE(writeFile(root + u"/alice/.hidden/secret.mp3"_s, mp3Frames(5)));
    ASSERT_TRUE(writeFile(root + u"/alice/.DS_Store"_s, QByteArray(4, 'x')));
    ASSERT_TRUE(writeFile(root + u"/bob/x.mp3"_s, mp3Frames(5)));
    ASSERT_TRUE(writeFile(root + u"/server/x.mp3"_s, mp3Frames(5))); // not a username anyone may have
    ASSERT_TRUE(writeFile(root + u"/.git/x"_s, QByteArray(1, 'x')));
    ASSERT_TRUE(writeFile(root + u"/readme.txt"_s, QByteArray(1, 'x')));

    QStringList skipped;
    const QList<LibraryPeer> peers = scanLibrary(root, &skipped);
    ASSERT_EQ(peers.size(), 2);
    EXPECT_EQ(skipped, QStringList{u"server"_s});
    const LibraryPeer &alice = peers.at(0);
    EXPECT_EQ(alice.username, u"alice"_s);
    EXPECT_EQ(alice.fileCount(), 4);
    ASSERT_EQ(alice.shares.directories.size(), 2);
    EXPECT_EQ(alice.shares.directories.at(0).path.display(), u"alice"_s);
    const slsk::SharedDirectory &album = alice.shares.directories.at(1);
    EXPECT_EQ(album.path.display(), u"alice\\Kel Varo\\Album"_s);
    ASSERT_EQ(album.files.size(), 3);
    EXPECT_EQ(album.files.at(0).name.display(), u"01 - One.flac"_s); // name order
    EXPECT_EQ(album.files.at(0).attributes.duration, 3U);
    EXPECT_EQ(album.files.at(0).extension, u"flac"_s);
    EXPECT_EQ(album.files.at(1).attributes.bitrate, 320U);
    EXPECT_TRUE(album.files.at(2).attributes.isEmpty()); // the cover
    const QByteArray virtualPath = slsk::RemotePath::fromText(u"alice\\Kel Varo\\Album\\02 - Two.mp3"_s).raw;
    ASSERT_TRUE(alice.files.contains(virtualPath));
    EXPECT_TRUE(alice.files.value(virtualPath).endsWith(u"/alice/Kel Varo/Album/02 - Two.mp3"_s));
    quint64 total = 0;
    for (const QString &local : alice.files) {
        total += static_cast<quint64>(QFileInfo(local).size());
    }
    EXPECT_EQ(alice.totalBytes, total);
    EXPECT_EQ(peers.at(1).username, u"bob"_s);
}

// --- search matching -------------------------------------------------------------------------------------------------

TEST(DevnetSearch, MatchesWholeWordsLikeSharingClients)
{
    const slsk::SharedFileList shares = sampleShares();
    const auto names = [&](const QString &query, int limit = 100) {
        QStringList found;
        const QList<slsk::FileEntry> entries = matchSearch(shares, query, limit);
        for (const slsk::FileEntry &entry : entries) {
            found.append(entry.name.display().section(u'\\', -1));
        }
        return found;
    };
    EXPECT_EQ(names(u"kel varo"_s),
              (QStringList{u"01 - Amber Engine.flac"_s, u"02 - Slow Comet.flac"_s, u"cover.jpg"_s}));
    EXPECT_EQ(names(u"KEL varo comet"_s), QStringList{u"02 - Slow Comet.flac"_s});  // any case
    EXPECT_EQ(names(u"kel varo -cover"_s).size(), 2);                               // excluded word
    EXPECT_TRUE(names(u"var"_s).isEmpty());                                         // whole words only
    EXPECT_EQ(names(u"*aro"_s).size(), 3);                                          // * matches inside words
    EXPECT_EQ(names(u"sønder signal"_s), QStringList{u"01 - Glass Signal.flac"_s}); // diacritics
    EXPECT_EQ(names(u"kel"_s).size(), 4);                                           // "Kel's" is the word "kel" too
    EXPECT_EQ(names(u"flac"_s, 2).size(), 2);                                       // capped
    EXPECT_TRUE(names(u"-cover"_s).isEmpty());                                      // exclusions alone find nothing
    EXPECT_TRUE(names(u""_s).isEmpty());
    // Results carry the full virtual path.
    const QList<slsk::FileEntry> found = matchSearch(shares, u"amber engine"_s, 10);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.at(0).name.display(),
              u"lossless_lena\\Kel Varo\\Kel Varo - Paper Tide (2011) [FLAC]\\01 - Amber Engine.flac"_s);
}

// --- descriptions ----------------------------------------------------------------------------------------------------

TEST(DevnetDescribe, NamesAndTheFieldsWorthReading)
{
    EXPECT_EQ(describe(slsk::ToServerMessage{slsk::toserver::FileSearch{12, u"kel varo"_s}}),
              u"FileSearch token=12 query=\"kel varo\""_s);
    EXPECT_EQ(describe(slsk::ToServerMessage{slsk::toserver::ServerPing{}}), u"ServerPing"_s);
    EXPECT_EQ(describe(slsk::PeerMessage{
                  slsk::peer::UploadDenied{slsk::RemotePath::fromText(u"a\\b.mp3"_s), u"Too many files"_s}}),
              u"UploadDenied path=\"a\\b.mp3\" reason=\"Too many files\""_s);
    slsk::fromserver::LoginReply refused;
    refused.reason = u"INVALIDPASS"_s;
    EXPECT_EQ(describeServerFrame(slsk::frame(refused)), u"LoginReply refused reason=INVALIDPASS"_s);
    EXPECT_EQ(describePeerFrame(slsk::frame(slsk::peer::PlaceInQueueResponse{slsk::RemotePath::fromText(u"x"_s), 3})),
              u"PlaceInQueueResponse path=\"x\" place=3"_s);
    EXPECT_TRUE(describePeerFrame(QByteArray(5, 'x')).startsWith(u"(incomplete frame"_s));
    EXPECT_TRUE(
        describePeerFrame(slsk::encodeFrame(slsk::CodeWidth::Word, 9999, QByteArray(3, 'x'))).contains(u"unknown"_s));
}

// --- behaviours ------------------------------------------------------------------------------------------------------

TEST(DevnetBehaviour, FromTheFileAndTheCommandLine)
{
    QStringList errors;
    Behaviours behaviours =
        parseBehaviours(u"# how some peers behave\n\nquinn queue 45\n  rita refuse Too many files  \n"
                        u"half line\nx queue soon\nserver refuse no\nquinn sing loud\n"_s,
                        &errors);
    ASSERT_EQ(behaviours.size(), 2);
    EXPECT_EQ(behaviours.value(u"quinn"_s).queue, std::chrono::seconds(45));
    EXPECT_TRUE(behaviours.value(u"quinn"_s).refuse.isEmpty());
    EXPECT_EQ(behaviours.value(u"rita"_s).refuse, u"Too many files"_s);
    EXPECT_EQ(errors, (QStringList{u"line 5: half line"_s, u"line 6: x queue soon"_s, u"line 7: server refuse no"_s,
                                   u"line 8: quinn sing loud"_s}));
    // The command line adds to it and wins.
    EXPECT_TRUE(addQueueOption(behaviours, u"quinn=5"_s));
    EXPECT_EQ(behaviours.value(u"quinn"_s).queue, std::chrono::seconds(5));
    EXPECT_FALSE(addQueueOption(behaviours, u"quinn"_s));
    EXPECT_FALSE(addQueueOption(behaviours, u"=5"_s));
    EXPECT_FALSE(addQueueOption(behaviours, u"quinn=-1"_s));
    EXPECT_TRUE(addRefuseOption(behaviours, u"bob"_s));
    EXPECT_EQ(behaviours.value(u"bob"_s).refuse, u"Too many files"_s);
    EXPECT_TRUE(addRefuseOption(behaviours, u"bob=Pending shutdown."_s));
    EXPECT_EQ(behaviours.value(u"bob"_s).refuse, u"Pending shutdown."_s);
    EXPECT_FALSE(addRefuseOption(behaviours, u"=Banned"_s));
    EXPECT_EQ(describeBehaviour(behaviours.value(u"quinn"_s)), u"queues each download 5 s"_s);
    EXPECT_EQ(describeBehaviour(behaviours.value(u"bob"_s)), u"refuses every download: Pending shutdown."_s);
    EXPECT_EQ(describeBehaviour(PeerBehaviour{}), u"default"_s);
}

// --- corpus plan -----------------------------------------------------------------------------------------------------

TEST(DevnetCorpus, PlanIsDeterministicVariedAndMessy)
{
    const CorpusPlan plan = planCorpus(1);
    const CorpusPlan again = planCorpus(1);
    ASSERT_EQ(plan.tracks.size(), again.tracks.size());
    for (qsizetype i = 0; i < plan.tracks.size(); ++i) {
        EXPECT_EQ(plan.tracks.at(i).path, again.tracks.at(i).path);
        EXPECT_EQ(plan.tracks.at(i).frequency, again.tracks.at(i).frequency);
    }
    EXPECT_NE(planCorpus(2).tracks.at(0).path, plan.tracks.at(0).path);
    EXPECT_EQ(plan.peers.size(), 7);

    QSet<QString> paths;
    QSet<int> formats;
    bool nonLatin = false;
    for (const PlannedTrack &track : plan.tracks) {
        EXPECT_FALSE(paths.contains(track.path)) << track.path.toStdString();
        paths.insert(track.path);
        formats.insert(static_cast<int>(track.format));
        EXPECT_TRUE(plan.peers.contains(track.path.section(u'/', 0, 0))) << track.path.toStdString();
        EXPECT_GE(track.seconds, 6);
        EXPECT_LE(track.seconds, 12);
        EXPECT_GE(track.frequency, 200.0);
        EXPECT_FALSE(track.path.contains(u':'));
        nonLatin = nonLatin || track.title == u"夜の信号"_s;
    }
    EXPECT_TRUE(nonLatin);
    EXPECT_GE(formats.size(), 6); // WAV may not come up for every seed
    EXPECT_TRUE(std::ranges::any_of(plan.extras, [](const PlannedExtra &e) { return e.cover; }));
    EXPECT_TRUE(std::ranges::any_of(plan.extras, [](const PlannedExtra &e) { return e.path.endsWith(u".cue"_s); }));
    EXPECT_TRUE(std::ranges::any_of(plan.extras, [](const PlannedExtra &e) { return e.path.endsWith(u".nfo"_s); }));
    // The same release turns up at several peers.
    std::map<QString, QSet<QString>> peersByTitle;
    for (const PlannedTrack &track : plan.tracks) {
        peersByTitle[track.artist + track.title].insert(track.path.section(u'/', 0, 0));
    }
    EXPECT_TRUE(std::ranges::any_of(peersByTitle, [](const auto &entry) { return entry.second.size() > 1; }));
}

TEST(DevnetCorpus, PlanHasItsMisbehavingPeersAndTrackLists)
{
    const CorpusPlan plan = planCorpus(1);
    EXPECT_TRUE(plan.peers.contains(u"queue_quinn"_s));
    EXPECT_TRUE(plan.peers.contains(u"refusing_rita"_s));
    QHash<QString, QByteArray> root;
    for (const PlannedExtra &extra : plan.extras) {
        if (!extra.path.contains(u'/')) {
            root.insert(extra.path, extra.text);
        }
    }
    ASSERT_TRUE(root.contains(behaviourFileName()));
    QStringList errors;
    const Behaviours behaviours = parseBehaviours(QString::fromUtf8(root.value(behaviourFileName())), &errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join(u", "_s).toStdString();
    EXPECT_EQ(behaviours.value(u"queue_quinn"_s).queue, std::chrono::seconds(45));
    EXPECT_EQ(behaviours.value(u"refusing_rita"_s).refuse, u"Too many files"_s);
    // Each of them alone shares one release, so its behaviour can't be avoided for those tracks.
    std::map<QString, QSet<QString>> peersByAlbum;
    for (const PlannedTrack &track : plan.tracks) {
        peersByAlbum[track.artist + u" - "_s + track.album].insert(track.path.section(u'/', 0, 0));
    }
    const QStringList misbehaving{u"queue_quinn"_s, u"refusing_rita"_s};
    for (const QString &peer : misbehaving) {
        EXPECT_TRUE(std::ranges::any_of(peersByAlbum, [&](const auto &entry) {
            return entry.second == QSet<QString>{peer};
        })) << peer.toStdString();
    }
    // Every shared track once in tracklist.txt; tracklist-missing.txt names none of them.
    const QStringList wanted = QString::fromUtf8(root.value(u"tracklist.txt"_s)).split(u'\n', Qt::SkipEmptyParts);
    QSet<QString> expected;
    for (const PlannedTrack &track : plan.tracks) {
        expected.insert(track.artist + u" - "_s + track.title);
    }
    EXPECT_EQ(QSet<QString>(wanted.cbegin(), wanted.cend()), expected);
    EXPECT_EQ(wanted.size(), expected.size());
    const QStringList missing =
        QString::fromUtf8(root.value(u"tracklist-missing.txt"_s)).split(u'\n', Qt::SkipEmptyParts);
    EXPECT_EQ(missing.size(), 3);
    for (const QString &line : missing) {
        EXPECT_FALSE(expected.contains(line)) << line.toStdString();
    }
}

TEST(DevnetCorpus, WrittenFilesProbeAsPlanned)
{
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) {
        GTEST_SKIP() << "ffmpeg isn't installed";
    }
    // One track of each format, shortened.
    const CorpusPlan full = planCorpus(3);
    CorpusPlan plan;
    QSet<int> seen;
    for (PlannedTrack track : full.tracks) {
        if (!seen.contains(static_cast<int>(track.format))) {
            seen.insert(static_cast<int>(track.format));
            track.seconds = 2;
            plan.tracks.append(track);
        }
    }
    plan.extras.append({u"lossless_lena/cover.jpg"_s, {}, true});
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(u"corpus"_s);
    QString error;
    ASSERT_TRUE(writeCorpus(plan, target, ffmpeg, &error)) << error.toStdString();
    for (const PlannedTrack &track : std::as_const(plan.tracks)) {
        const slsk::FileAttributes a = probeAudio(QDir(target).filePath(track.path));
        EXPECT_EQ(a.duration, 2U) << track.path.toStdString();
        switch (track.format) {
        case AudioFormat::Mp3Cbr320:
            EXPECT_EQ(a.bitrate, 320U);
            EXPECT_EQ(a.vbr, false);
            break;
        case AudioFormat::Mp3Cbr192:
            EXPECT_EQ(a.bitrate, 192U);
            break;
        case AudioFormat::Mp3Vbr0:
            EXPECT_EQ(a.vbr, true);
            EXPECT_GT(a.bitrate.value_or(0), 150U); // the noise keeps V0 near a real file's rate
            break;
        case AudioFormat::Flac24:
            EXPECT_EQ(a.sampleRate, 96000U);
            EXPECT_EQ(a.bitDepth, 24U);
            break;
        case AudioFormat::Flac16:
        case AudioFormat::Aiff16:
        case AudioFormat::Wav16:
            EXPECT_EQ(a.sampleRate, 44100U);
            EXPECT_EQ(a.bitDepth, 16U);
            break;
        }
    }
    EXPECT_TRUE(QFileInfo::exists(QDir(target).filePath(u"lossless_lena/cover.jpg"_s)));
    // Never over existing files.
    EXPECT_FALSE(writeCorpus(plan, target, ffmpeg, &error));
    EXPECT_TRUE(error.contains(u"isn't empty"_s));
}

// --- the network, end to end -----------------------------------------------------------------------------------------

namespace {

class MemoryStorage : public slsk::DownloadStorage
{
public:
    QIODevice *openDownload(const slsk::DownloadInfo &download) override
    {
        auto &buffer = m_open[download.id];
        buffer = std::make_unique<QBuffer>(&contents[download.request.path.raw]);
        return buffer->open(QIODevice::WriteOnly | QIODevice::Append) ? buffer.get() : nullptr;
    }
    void closeDownload(const slsk::DownloadInfo &download, QIODevice *device) override
    {
        device->close();
        m_open.erase(download.id);
    }
    QHash<QByteArray, QByteArray> contents;

private:
    std::map<slsk::TransferId, std::unique_ptr<QBuffer>> m_open;
};

class DevnetNetworkTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        QRandomGenerator random(7);
        m_content = QByteArray(300'000, Qt::Uninitialized);
        for (char &c : m_content) {
            c = static_cast<char>(random.bounded(256));
        }
        ASSERT_TRUE(writeFile(m_dir.filePath(u"lena/Kel Varo/Paper Tide/01 - Amber Engine.flac"_s), m_content));
        ASSERT_TRUE(writeFile(m_dir.filePath(u"carl/Kel_Varo-Paper_Tide-WEB-2011/01-kel_varo-amber_engine.mp3"_s),
                              mp3Frames(40)));
    }

    // Starts the network with `options` and logs our client in as "selecta".
    void startAndLogIn(NetworkOptions options)
    {
        options.port = 0;
        options.searchDelay = 100ms;
        m_network = std::make_unique<DevNetwork>(options);
        QObject::connect(m_network.get(), &DevNetwork::logLine, m_network.get(),
                         [log = &m_log](const QString &line) { log->append(line); });
        QString error;
        ASSERT_TRUE(m_network->start(&error)) << error.toStdString();
        const QList<LibraryPeer> library = scanLibrary(m_dir.path());
        for (const LibraryPeer &peer : library) {
            ASSERT_TRUE(m_network->addPeer(peer, &error)) << error.toStdString();
        }
        m_peers.setTimeouts({1s, 1s, 5s, 1s, 3s});
        m_transfers.setDownloadStorage(&m_storage);
        ASSERT_TRUE(m_peers.listen(0, QHostAddress(QHostAddress::LocalHost)));
        m_session.setListenPort(m_peers.listenPort());
        ASSERT_TRUE(m_session.connectToServer(QHostAddress(QHostAddress::LocalHost), m_network->port(),
                                              permit(Operation::Login)));
        ASSERT_TRUE(m_session.login(u"selecta"_s, u"pw"_s));
        ASSERT_TRUE(waitFor([&] { return m_session.isLoggedIn(); }));
        settle(100ms); // the server learns our listening port
    }

    QTemporaryDir m_dir;
    QByteArray m_content;
    QStringList m_log;
    std::unique_ptr<DevNetwork> m_network;
    slsk::ServerSession m_session;
    TestPermits m_permits;
    MemoryStorage m_storage;
    slsk::PeerNetwork m_peers{m_session, m_permits};
    slsk::TransferManager m_transfers{m_peers, m_session};
};

} // namespace

TEST_F(DevnetNetworkTest, SearchAndDownloadAtThePeersSpeed)
{
    NetworkOptions options;
    options.uploadRate = qint64{4} * 1024 * 1024;
    options.queueWait = 200ms;
    startAndLogIn(options);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(m_network->peerNames(), (QStringList{u"carl"_s, u"lena"_s}));

    const QSignalSpy results(&m_peers, &slsk::PeerNetwork::searchResponseReceived);
    m_peers.openSearch(77);
    ASSERT_TRUE(m_session.fileSearch(77, u"kel varo amber"_s, permit(Operation::Search)));
    ASSERT_TRUE(waitFor([&] { return results.size() == 2; }, 5s));
    QSet<QString> from;
    for (const auto &result : results) {
        const auto response = result.at(1).value<slsk::peer::FileSearchResponse>();
        EXPECT_EQ(response.token, 77U);
        ASSERT_EQ(response.results.size(), 1);
        from.insert(result.at(0).toString());
    }
    EXPECT_EQ(from, (QSet<QString>{u"lena"_s, u"carl"_s}));

    const slsk::RemotePath path = slsk::RemotePath::fromText(u"lena\\Kel Varo\\Paper Tide\\01 - Amber Engine.flac"_s);
    const QSignalSpy changes(&m_transfers, &slsk::TransferManager::downloadChanged);
    const slsk::TransferId id =
        m_transfers.download(slsk::DownloadRequest{u"lena"_s, path, static_cast<quint64>(m_content.size()), false},
                             permit(Operation::QueueDownload));
    ASSERT_NE(id, 0U);
    ASSERT_TRUE(waitFor(
        [&] { return !changes.isEmpty() && changes.constLast().at(0).value<slsk::DownloadInfo>().isTerminal(); }, 10s));
    EXPECT_EQ(changes.constLast().at(0).value<slsk::DownloadInfo>().state, slsk::DownloadState::Completed);
    EXPECT_EQ(m_storage.contents.value(path.raw), m_content);
    // Every message was logged, and the upload too.
    EXPECT_TRUE(
        std::ranges::any_of(m_log, [](const QString &l) { return l.contains(u"server <- selecta  FileSearch"_s); }));
    EXPECT_TRUE(
        std::ranges::any_of(m_log, [](const QString &l) { return l.contains(u"lena <- selecta  QueueUpload"_s); }));
    // The uploader sees our close a moment later.
    EXPECT_TRUE(waitFor([&] {
        return std::ranges::any_of(
            m_log, [](const QString &l) { return l.contains(u"upload of"_s) && l.contains(u"complete"_s); });
    }));
}

TEST_F(DevnetNetworkTest, RefusalsAndUnknownFiles)
{
    NetworkOptions options;
    options.denyPercent = 100;
    startAndLogIn(options);
    ASSERT_FALSE(HasFatalFailure());
    const QSignalSpy changes(&m_transfers, &slsk::TransferManager::downloadChanged);
    const auto fails = [&](const QString &user, const QString &file) {
        const slsk::TransferId id = m_transfers.download(
            slsk::DownloadRequest{user, slsk::RemotePath::fromText(file), 1, false}, permit(Operation::QueueDownload));
        EXPECT_TRUE(waitFor([&] {
            return std::ranges::any_of(changes, [id](const QList<QVariant> &change) {
                const auto info = change.at(0).value<slsk::DownloadInfo>();
                return info.id == id && info.state == slsk::DownloadState::Failed;
            });
        }));
        for (const auto &change : changes) {
            const auto info = change.at(0).value<slsk::DownloadInfo>();
            if (info.id == id && info.isTerminal()) {
                return info.rejection ? info.rejection->text : QString();
            }
        }
        return QString();
    };
    const QStringList realistic{u"Too many files"_s, u"Too many megabytes"_s, u"Pending shutdown."_s, u"Banned"_s};
    EXPECT_TRUE(realistic.contains(fails(u"lena"_s, u"lena\\Kel Varo\\Paper Tide\\01 - Amber Engine.flac"_s)));
    EXPECT_EQ(fails(u"lena"_s, u"lena\\nothing here.mp3"_s), u"File not shared."_s);
}

TEST_F(DevnetNetworkTest, PlaceInQueueWhileWaiting)
{
    NetworkOptions options;
    options.queueWait = 5s;
    startAndLogIn(options);
    ASSERT_FALSE(HasFatalFailure());
    const QSignalSpy changes(&m_transfers, &slsk::TransferManager::downloadChanged);
    const slsk::TransferId id = m_transfers.download(
        slsk::DownloadRequest{
            u"carl"_s,
            slsk::RemotePath::fromText(u"carl\\Kel_Varo-Paper_Tide-WEB-2011\\01-kel_varo-amber_engine.mp3"_s), 0,
            false},
        permit(Operation::QueueDownload));
    settle(300ms);
    ASSERT_TRUE(m_transfers.requestPlaceInQueue(id, permit(Operation::PlaceInQueue)));
    EXPECT_TRUE(waitFor([&] {
        return std::ranges::any_of(changes, [id](const QList<QVariant> &change) {
            const auto info = change.at(0).value<slsk::DownloadInfo>();
            return info.id == id && info.placeInQueue == 1U;
        });
    }));
}

TEST_F(DevnetNetworkTest, OnePeerQueuesForAWhileAnotherRefuses)
{
    NetworkOptions options;
    options.behaviours.insert(u"lena"_s, PeerBehaviour{std::chrono::seconds(2), {}});
    options.behaviours.insert(u"carl"_s, PeerBehaviour{std::nullopt, u"Pending shutdown."_s});
    startAndLogIn(options);
    ASSERT_FALSE(HasFatalFailure());
    const QSignalSpy changes(&m_transfers, &slsk::TransferManager::downloadChanged);
    const auto last = [&](slsk::TransferId id) {
        std::optional<slsk::DownloadInfo> found;
        for (const auto &change : changes) {
            if (const auto info = change.at(0).value<slsk::DownloadInfo>(); info.id == id) {
                found = info;
            }
        }
        return found;
    };
    const slsk::RemotePath queued = slsk::RemotePath::fromText(u"lena\\Kel Varo\\Paper Tide\\01 - Amber Engine.flac"_s);
    QElapsedTimer clock;
    clock.start();
    const slsk::TransferId waiting =
        m_transfers.download(slsk::DownloadRequest{u"lena"_s, queued, static_cast<quint64>(m_content.size()), false},
                             permit(Operation::QueueDownload));
    const slsk::TransferId refused = m_transfers.download(
        slsk::DownloadRequest{
            u"carl"_s,
            slsk::RemotePath::fromText(u"carl\\Kel_Varo-Paper_Tide-WEB-2011\\01-kel_varo-amber_engine.mp3"_s), 0,
            false},
        permit(Operation::QueueDownload));
    // The refusal comes at once, with its reason.
    ASSERT_TRUE(waitFor([&] { return last(refused) && last(refused)->isTerminal(); }));
    EXPECT_EQ(last(refused)->state, slsk::DownloadState::Failed);
    ASSERT_TRUE(last(refused)->rejection.has_value());
    EXPECT_EQ(last(refused)->rejection->text, u"Pending shutdown."_s);
    // The queued one waits its turn (first in line), then arrives.
    settle(300ms);
    EXPECT_EQ(last(waiting)->state, slsk::DownloadState::Queued);
    ASSERT_TRUE(m_transfers.requestPlaceInQueue(waiting, permit(Operation::PlaceInQueue)));
    EXPECT_TRUE(waitFor([&] { return last(waiting)->placeInQueue == 1U; }));
    ASSERT_TRUE(waitFor([&] { return last(waiting)->isTerminal(); }, 10s));
    EXPECT_EQ(last(waiting)->state, slsk::DownloadState::Completed);
    EXPECT_GE(clock.elapsed(), 1900);
    EXPECT_EQ(m_storage.contents.value(queued.raw), m_content);
    // The searcher sees the queueing peer's lack of a free slot.
    const QSignalSpy results(&m_peers, &slsk::PeerNetwork::searchResponseReceived);
    m_peers.openSearch(5);
    ASSERT_TRUE(m_session.fileSearch(5, u"amber engine"_s, permit(Operation::Search)));
    ASSERT_TRUE(waitFor([&] { return results.size() == 2; }, 5s));
    for (const auto &result : results) {
        if (result.at(0).toString() == u"lena"_s) {
            EXPECT_FALSE(result.at(1).value<slsk::peer::FileSearchResponse>().slotFree);
        }
    }
}

TEST_F(DevnetNetworkTest, OnlyTheGivenAccountLogsIn)
{
    NetworkOptions options;
    options.port = 0;
    options.user = u"maintainer"_s;
    options.password = u"secret"_s;
    DevNetwork network(options);
    QString error;
    ASSERT_TRUE(network.start(&error)) << error.toStdString();
    slsk::ServerSession session;
    const QSignalSpy ended(&session, &slsk::ServerSession::ended);
    ASSERT_TRUE(
        session.connectToServer(QHostAddress(QHostAddress::LocalHost), network.port(), permit(Operation::Login)));
    ASSERT_TRUE(session.login(u"maintainer"_s, u"wrong"_s));
    ASSERT_TRUE(waitFor([&] { return !ended.isEmpty(); }));
    EXPECT_EQ(session.endReason(), slsk::ServerSession::EndReason::LoginRejected);
    EXPECT_EQ(session.loginRejection().reason, u"INVALIDPASS"_s);

    slsk::ServerSession good;
    ASSERT_TRUE(good.connectToServer(QHostAddress(QHostAddress::LocalHost), network.port(), permit(Operation::Login)));
    ASSERT_TRUE(good.login(u"maintainer"_s, u"secret"_s));
    EXPECT_TRUE(waitFor([&] { return good.isLoggedIn(); }));
}
