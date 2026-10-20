// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/FileRanking.h"

#include <QSet>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::FileRanking;
using selecta::core::GetLine;
using selecta::core::SearchHit;
using selecta::core::SearchOutcome;

namespace {

selecta::core::SearchFilter preset(QStringView id)
{
    return selecta::core::builtInPreset(id).value().filter;
}

GetLine resolvedLine()
{
    GetLine line;
    line.raw = u"Okuma Drift - Neon Rain"_s;
    line.artist = u"Okuma Drift"_s;
    line.title = u"Neon Rain"_s;
    selecta::meta::Release release;
    release.artists = {u"Okuma Drift"_s};
    release.title = u"Neon Rain"_s;
    release.album = u"Nightfall"_s;
    release.durationMs = 372'000;
    line.release = release;
    line.releaseState = GetLine::ReleaseState::Resolved;
    return line;
}

SearchHit hit(const QString &peer, const QString &path, const QString &extension, std::optional<quint32> bitrate,
              quint32 durationSec, quint64 size, bool freeSlot = true)
{
    SearchHit result;
    result.peer = peer;
    result.path = path.toUtf8();
    result.extension = extension;
    result.size = size;
    result.attributes.bitrate = bitrate;
    result.attributes.duration = durationSec;
    if (extension == u"flac") {
        result.attributes.sampleRate = 44'100;
        result.attributes.bitDepth = 16;
    }
    result.freeSlot = freeSlot;
    return result;
}

} // namespace

TEST(FileRanking, TheReleasesLengthAndQualityDecide)
{
    SearchOutcome outcome;
    outcome.hits = {
        hit(u"mp3_peer"_s, u"@@a\\Music\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000),
        hit(u"flac_peer"_s, u"@@b\\Okuma Drift\\Nightfall\\03 - Neon Rain.flac"_s, u"flac"_s, std::nullopt, 372,
            42'000'000),
        hit(u"low_peer"_s, u"@@c\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 192, 372, 8'900'000),
        hit(u"remix_peer"_s, u"@@d\\Okuma Drift - Neon Rain (Ines Varga Remix).mp3"_s, u"mp3"_s, 320, 401, 16'000'000),
        hit(u"cover_peer"_s, u"@@e\\Okuma Drift - Neon Rain\\cover.jpg"_s, u"jpg"_s, std::nullopt, 0, 120'000),
    };
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    EXPECT_EQ(ranked.considered, 4);                 // the cover isn't audio
    EXPECT_EQ(ranked.candidates, 3);                 // the 192 kbps file can't make an MP3 320
    ASSERT_EQ(ranked.files.size(), 2);               // the remix is another version
    EXPECT_EQ(ranked.files[0].peer, u"flac_peer"_s); // lossless beats MP3 320 at the same fit
    EXPECT_TRUE(ranked.files[0].fitsRelease);
    EXPECT_EQ(ranked.files[1].peer, u"mp3_peer"_s);
    EXPECT_TRUE(ranked.files[1].fitsRelease);
}

TEST(FileRanking, AWrongLengthIsAtBestToCheck)
{
    SearchOutcome outcome;
    outcome.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 300, 12'000'000)};
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    if (!ranked.files.isEmpty()) {
        EXPECT_FALSE(ranked.files[0].fitsRelease);
    }
}

TEST(FileRanking, OneFilePerPeer)
{
    SearchOutcome outcome;
    outcome.hits = {
        hit(u"same"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000),
        hit(u"same"_s, u"@@a\\Okuma Drift - Neon Rain.flac"_s, u"flac"_s, std::nullopt, 372, 42'000'000),
    };
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kAnyQualityPreset));
    ASSERT_EQ(ranked.files.size(), 1);
    EXPECT_EQ(ranked.files[0].extension, u"flac"_s);
}

TEST(FileRanking, LosslessOnlyMeansLosslessOnly)
{
    SearchOutcome outcome;
    outcome.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000)};
    EXPECT_TRUE(FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kLosslessPreset)).files.isEmpty());
}

TEST(FileRanking, AnyFilterDecidesTheCandidates)
{
    // A preset of the user's (SRCH-8): only peers with a free slot, and nothing from slow ones.
    SearchOutcome outcome;
    SearchHit busy = hit(u"busy"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000);
    busy.freeSlot = false;
    SearchHit slow = hit(u"slow"_s, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000);
    slow.averageSpeed = 20'000;
    SearchHit fine = hit(u"fine"_s, u"@@c\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000);
    fine.averageSpeed = 2'000'000;
    outcome.hits = {busy, slow, fine};
    selecta::core::SearchFilter filter = preset(selecta::core::kMp3OrLosslessPreset);
    filter.freeSlotOnly = true;
    filter.minSpeedKBps = 100;
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, filter);
    EXPECT_EQ(ranked.considered, 3);
    EXPECT_EQ(ranked.candidates, 1);
    ASSERT_EQ(ranked.files.size(), 1);
    EXPECT_EQ(ranked.files[0].peer, u"fine"_s);
}

TEST(FileRanking, WithoutAReleaseTheLineStandsIn)
{
    GetLine line;
    line.raw = u"Okuma Drift - Neon Rain"_s;
    line.artist = u"Okuma Drift"_s;
    line.title = u"Neon Rain"_s;
    line.releaseState = GetLine::ReleaseState::None;
    SearchOutcome outcome;
    outcome.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000)};
    const auto ranked = FileRanking::rank(line, outcome, preset(selecta::core::kMp3OrLosslessPreset));
    ASSERT_EQ(ranked.files.size(), 1);
}

TEST(FileRanking, QueriesComeFromTheRelease)
{
    QString why;
    const QString query = FileRanking::query(resolvedLine(), {}, &why);
    EXPECT_TRUE(query.contains(u"okuma"_s)) << query.toStdString();
    EXPECT_TRUE(query.contains(u"neon"_s)) << query.toStdString();
    GetLine blocked = resolvedLine();
    EXPECT_TRUE(FileRanking::query(blocked, {u"neon"_s}, &why).isEmpty());
    EXPECT_FALSE(why.isEmpty());
}

TEST(FileRanking, PeersTakenFromLessComeFirstAllElseEqual)
{
    // FAIR-5: the same file from two peers with free slots; the one we took less from today ranks first.
    SearchOutcome outcome;
    outcome.hits = {hit(u"busy_today"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000),
                    hit(u"fresh"_s, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000)};
    const auto load = [](const QString &peer) { return peer == u"busy_today"_s ? 12 : 0; };
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset), load);
    ASSERT_EQ(ranked.files.size(), 2);
    EXPECT_EQ(ranked.files[0].peer, u"fresh"_s);
    // Without the load, the order is by name: the tie-break is the load, nothing else.
    const auto plain = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    EXPECT_EQ(plain.files[0].peer, u"busy_today"_s);
}

TEST(FileRanking, NothingInTheQualityWantedGivesTheTracksNearMisses)
{
    SearchOutcome outcome;
    outcome.hits = {
        hit(u"low_peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 192, 372, 8'900'000),
        hit(u"lower_peer"_s, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 128, 372, 5'950'000),
        hit(u"low_peer"_s, u"@@a\\Okuma Drift - Neon Rain (2).mp3"_s, u"mp3"_s, 128, 372, 5'950'000), // its worse copy
        hit(u"aac_peer"_s, u"@@c\\Okuma Drift - Neon Rain.m4a"_s, u"m4a"_s, 256, 372, 11'900'000),
        hit(u"remix_peer"_s, u"@@d\\Okuma Drift - Neon Rain (Ines Varga Remix).mp3"_s, u"mp3"_s, 192, 401,
            9'600'000),
        hit(u"other_peer"_s, u"@@e\\Halden Rue - Tidewater.mp3"_s, u"mp3"_s, 192, 401, 9'600'000),
    };
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    EXPECT_TRUE(ranked.files.isEmpty());
    EXPECT_EQ(ranked.considered, 6);
    EXPECT_EQ(ranked.candidates, 0);
    // The track's files, below the quality wanted: not the remix, not another track. Each peer's best, best first.
    EXPECT_EQ(ranked.belowQuality, 4);
    ASSERT_EQ(ranked.nearMisses.size(), 3);
    EXPECT_EQ(ranked.nearMisses[0].peer, u"aac_peer"_s); // AAC 256 ranks above MP3 192
    EXPECT_EQ(ranked.nearMisses[1].peer, u"low_peer"_s);
    EXPECT_EQ(ranked.nearMisses[1].bitrate, 192);
    EXPECT_EQ(ranked.nearMisses[2].peer, u"lower_peer"_s);
    for (const selecta::core::FileChoice &miss : ranked.nearMisses) {
        EXPECT_TRUE(miss.fitsRelease) << miss.peer.toStdString();
        EXPECT_FALSE(miss.shortfall.isEmpty()) << miss.peer.toStdString();
    }
    EXPECT_EQ(ranked.nearMisses[1].shortfall, u"under 320 kbps"_s);
    EXPECT_TRUE(ranked.nearMisses[0].shortfall.contains(u"AAC"_s)) << ranked.nearMisses[0].shortfall.toStdString();
}

TEST(FileRanking, NearMissesOnlyWhenNothingPasses)
{
    SearchOutcome outcome;
    outcome.hits = {hit(u"good"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000),
                    hit(u"low"_s, u"@@b\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 192, 372, 8'900'000)};
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    ASSERT_EQ(ranked.files.size(), 1);
    EXPECT_TRUE(ranked.files[0].shortfall.isEmpty());
    EXPECT_TRUE(ranked.nearMisses.isEmpty());
    EXPECT_EQ(ranked.belowQuality, 0);
}

TEST(FileRanking, AtMostFiveNearMissesOnePerPeer)
{
    SearchOutcome outcome;
    for (int i = 0; i < 8; ++i) {
        outcome.hits.append(hit(u"peer%1"_s.arg(i), u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s,
                                static_cast<quint32>(128 + (8 * i)), 372, 7'000'000));
    }
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    EXPECT_EQ(ranked.belowQuality, 8);
    ASSERT_EQ(ranked.nearMisses.size(), FileRanking::kMaxChoices);
    QSet<QString> peers;
    for (const selecta::core::FileChoice &miss : ranked.nearMisses) {
        peers.insert(miss.peer);
    }
    EXPECT_EQ(peers.size(), FileRanking::kMaxChoices);
}

TEST(FileRanking, ShortfallsSayWhatThePresetWanted)
{
    // Lossless only turns an MP3 320 down for its format; hi-res a 16-bit FLAC for its depth.
    SearchOutcome mp3;
    mp3.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000)};
    const auto lossless = FileRanking::rank(resolvedLine(), mp3, preset(selecta::core::kLosslessPreset));
    ASSERT_EQ(lossless.nearMisses.size(), 1);
    EXPECT_EQ(lossless.nearMisses[0].shortfall, u"MP3 isn't a format wanted"_s);

    SearchOutcome flac;
    flac.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.flac"_s, u"flac"_s, std::nullopt, 372, 42'000'000)};
    const auto hiRes = FileRanking::rank(resolvedLine(), flac, preset(selecta::core::kHiResPreset));
    ASSERT_EQ(hiRes.nearMisses.size(), 1);
    EXPECT_EQ(hiRes.nearMisses[0].shortfall, u"under 24-bit"_s);

    // A file that doesn't say its quality falls to strict mode.
    SearchOutcome silent;
    silent.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, std::nullopt, 372, 14'900'000)};
    const auto unknown = FileRanking::rank(resolvedLine(), silent, preset(selecta::core::kMp3OrLosslessPreset));
    ASSERT_EQ(unknown.nearMisses.size(), 1);
    EXPECT_EQ(unknown.nearMisses[0].shortfall, u"its quality isn't stated"_s);

    // A user's preset: a busy peer and a slow one.
    selecta::core::SearchFilter filter = preset(selecta::core::kMp3OrLosslessPreset);
    filter.freeSlotOnly = true;
    SearchOutcome busy;
    busy.hits = {hit(u"busy"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000, false)};
    const auto noSlot = FileRanking::rank(resolvedLine(), busy, filter);
    ASSERT_EQ(noSlot.nearMisses.size(), 1);
    EXPECT_EQ(noSlot.nearMisses[0].shortfall, u"no free slot"_s);
}

TEST(FileRanking, NothingOfTheTrackGivesNoNearMisses)
{
    SearchOutcome outcome;
    outcome.hits = {hit(u"other"_s, u"@@e\\Halden Rue - Tidewater.mp3"_s, u"mp3"_s, 192, 401, 9'600'000)};
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, preset(selecta::core::kMp3OrLosslessPreset));
    EXPECT_TRUE(ranked.files.isEmpty());
    EXPECT_TRUE(ranked.nearMisses.isEmpty());
    EXPECT_EQ(ranked.belowQuality, 0);
    EXPECT_EQ(ranked.considered, 1);
}
