// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/FileRanking.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::FileRanking;
using selecta::core::GetLine;
using selecta::core::QualityWanted;
using selecta::core::SearchHit;
using selecta::core::SearchOutcome;

namespace {

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
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, QualityWanted::Mp3_320OrLossless);
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
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, QualityWanted::Mp3_320OrLossless);
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
    const auto ranked = FileRanking::rank(resolvedLine(), outcome, QualityWanted::Any);
    ASSERT_EQ(ranked.files.size(), 1);
    EXPECT_EQ(ranked.files[0].extension, u"flac"_s);
}

TEST(FileRanking, LosslessOnlyMeansLosslessOnly)
{
    SearchOutcome outcome;
    outcome.hits = {hit(u"peer"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, u"mp3"_s, 320, 372, 14'900'000)};
    EXPECT_TRUE(FileRanking::rank(resolvedLine(), outcome, QualityWanted::LosslessOnly).files.isEmpty());
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
    const auto ranked = FileRanking::rank(line, outcome, QualityWanted::Mp3_320OrLossless);
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
