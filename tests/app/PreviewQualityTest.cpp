// SPDX-License-Identifier: GPL-3.0-or-later
// PREV-9: a peer's complete preview gets the spectrum check, and the player bar shows what it found; YouTube previews
// aren't judged, and a new preview or Stop drops what belonged to the previous one.
#include "SyntheticAudio.h"
#include "app/PlayerBar.h"
#include "app/Preview.h"
#include "audio/AnalysisCache.h"

#include <QLabel>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::PlayerBar;
using selecta::app::PreviewController;
namespace audio = selecta::audio;
namespace synth = selecta::testing;

namespace {

const QString kPeer = u"peer|vinyl_ghost|Music\\Okuma Drift - Neon Rain.flac|1234"_s;

selecta::app::PreviewRequest request(const QByteArray &file, const QString &identity)
{
    return {
        std::make_shared<audio::MemorySource>(file), {u"Neon Rain"_s, u"Okuma Drift"_s, u"vinyl_ghost"_s}, identity};
}

// A FLAC with nothing above 16 kHz, as a lossy encoder leaves it.
QByteArray lowPassedFlac()
{
    return synth::makeFlac(synth::quantised(synth::brickwallNoise(16000.0, 6.0, 9, 44100, 2, 0.1), 16));
}

class PreviewQualityTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }

    QTemporaryDir dir;
    std::shared_ptr<audio::AnalysisCache> cache = std::make_shared<audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    PreviewController controller{selecta::playback::AudioOutput::createSilent(), cache}; // never the audio device
    PlayerBar bar{controller};
    QLabel *badge = bar.findChild<QLabel *>(u"player.quality"_s);
};

} // namespace

TEST_F(PreviewQualityTest, APeersCompleteFileIsCheckedAndTheBadgeSaysWhatItFound)
{
    ASSERT_NE(badge, nullptr);
    EXPECT_TRUE(badge->isHidden());
    controller.preview(request(lowPassedFlac(), kPeer));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.quality().has_value(); }, 15000));
    EXPECT_TRUE(controller.quality()->hasFinding(audio::QualityFinding::LikelyFromLossy));
    // A symbol and words (UI-4); "likely", never "fake".
    EXPECT_FALSE(badge->isHidden());
    EXPECT_EQ(badge->text(), u"⚠ Likely from a lossy file"_s);
    EXPECT_TRUE(badge->toolTip().startsWith(u"Likely from a lossy source (the spectrum stops at"_s))
        << badge->toolTip().toStdString();
    EXPECT_FALSE(badge->toolTip().contains(u"fake"_s, Qt::CaseInsensitive));
    // Kept under the preview's name: hearing the same file again finds it there.
    EXPECT_TRUE(cache->loadQuality(audio::CacheKey::forName(kPeer)).has_value());

    // The next preview starts without the previous one's badge.
    controller.preview(request(lowPassedFlac(), u"youtube|dQw4w9WgXcQ"_s));
    EXPECT_FALSE(controller.quality().has_value());
    EXPECT_TRUE(badge->isHidden());
}

TEST_F(PreviewQualityTest, YouTubeAndLocalPreviewsArentJudged)
{
    for (const QString &identity : {u"youtube|dQw4w9WgXcQ"_s, u"file|/Music/Neon Rain.flac|1|2"_s, QString()}) {
        controller.preview(request(lowPassedFlac(), identity));
        ASSERT_TRUE(QTest::qWaitFor([&] { return controller.waveform().isComplete(); }, 15000));
        QTest::qWait(300); // a check would have had time to run
        EXPECT_FALSE(controller.quality().has_value()) << identity.toStdString();
        EXPECT_TRUE(badge->isHidden());
    }
}

TEST_F(PreviewQualityTest, AGenuineFileGetsNoBadge)
{
    const QByteArray flac = synth::makeFlac(synth::quantised(synth::whiteNoise(6.0, 4, 44100, 2, 0.3), 16));
    controller.preview(request(flac, kPeer));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.quality().has_value(); }, 15000));
    EXPECT_TRUE(controller.quality()->findings.isEmpty());
    EXPECT_GT(controller.quality()->cutoffHz, 21000);
    EXPECT_TRUE(badge->isHidden());
}

TEST_F(PreviewQualityTest, StopDropsTheCheck)
{
    controller.preview(request(lowPassedFlac(), kPeer));
    ASSERT_TRUE(QTest::qWaitFor([&] { return controller.quality().has_value(); }, 15000));
    controller.stop();
    EXPECT_FALSE(controller.quality().has_value());
    EXPECT_TRUE(badge->isHidden());
    // Stopped before the file was even analysed: nothing arrives later.
    controller.preview(request(lowPassedFlac(), u"peer|vinyl_ghost|other.flac|99"_s));
    controller.stop();
    QTest::qWait(1500);
    EXPECT_FALSE(controller.quality().has_value());
}
