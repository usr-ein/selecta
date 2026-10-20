// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/Player.h"

#include "SyntheticAudio.h"
#include "audio/ByteSource.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::audio::MemorySource;
using selecta::playback::AudioOutput;
using selecta::playback::Player;
using selecta::playback::PlayerTrack;
using State = Player::State;
namespace synth = selecta::testing;

namespace {

constexpr int kRate = 48'000;
constexpr std::ptrdiff_t kFadedBy = 600; // floats: 300 frames, about 6 ms, when every fade is over

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QTest::qWait(2);
    }
    return true;
}

std::shared_ptr<MemorySource> wavSource(const synth::Pcm &pcm, double availableFraction = 1.0)
{
    const QByteArray wav = synth::makeWav(pcm, {.bits = 32, .floatingPoint = true});
    if (availableFraction >= 1.0) {
        return std::make_shared<MemorySource>(wav);
    }
    const auto available = static_cast<qint64>(static_cast<double>(wav.size()) * availableFraction);
    return std::make_shared<MemorySource>(wav, available);
}

PlayerTrack track(const QString &title)
{
    return {title, u"Test Artist"_s, u"Your Inbox"_s};
}

// An output the test drives by hand: every pull() runs the render callback once and returns what it wrote.
class ManualOutput final : public AudioOutput
{
public:
    int sampleRate() const override { return kRate; }
    bool start(Render render) override
    {
        if (refuse) {
            return false;
        }
        m_render = std::move(render);
        return true;
    }
    void stop() override { m_render = {}; }
    bool isRunning() const override { return static_cast<bool>(m_render); }
    int latencyFrames() const override { return 0; }
    QString error() const override { return u"the device is gone"_s; }

    std::vector<float> pull(int frames)
    {
        std::vector<float> samples(static_cast<std::size_t>(frames) * 2, 1.0F); // garbage that must be overwritten
        if (m_render) {
            m_render(samples.data(), frames);
        }
        return samples;
    }

    bool refuse = false;

private:
    Render m_render;
};

double rmsOf(const std::vector<float> &samples, std::size_t from = 0)
{
    double sum = 0.0;
    for (std::size_t i = from; i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return samples.size() > from ? std::sqrt(sum / static_cast<double>(samples.size() - from)) : 0.0;
}

float peakOf(const std::vector<float> &samples)
{
    float peak = 0.0F;
    for (const float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

class PlayerTest : public ::testing::Test
{
protected:
    Player player{AudioOutput::createSilent(kRate)};

    bool reaches(State state, int timeoutMs = 5000)
    {
        return waitFor([this, state] { return player.state() == state; }, timeoutMs);
    }
};

// The same, with an output the test pulls by hand.
class PlayerRenderTest : public ::testing::Test
{
protected:
    PlayerRenderTest()
        : output(new ManualOutput)
        , player(std::unique_ptr<AudioOutput>(output))
    {}

    // Pulls blocks of 480 frames (10 ms), letting the decoder thread run between them, until one carries sound.
    std::vector<float> pullUntilAudible(int maxBlocks = 300)
    {
        for (int block = 0; block < maxBlocks; ++block) {
            std::vector<float> samples = output->pull(480);
            if (peakOf(samples) > 0.0F) {
                return samples;
            }
            QTest::qWait(2);
        }
        return {};
    }

    ManualOutput *output; // owned by the player
    Player player;
};

} // namespace

TEST_F(PlayerTest, PlaysACompleteFileToTheEnd)
{
    QSignalSpy states(&player, &Player::stateChanged);
    const QSignalSpy tracks(&player, &Player::trackChanged);
    player.play(wavSource(synth::sine(440.0, 0.6, 44'100)), track(u"Short"_s));
    EXPECT_EQ(player.state(), State::Loading);
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(player.track().title, u"Short"_s);

    ASSERT_TRUE(reaches(State::Playing));
    EXPECT_NEAR(player.duration(), 0.6, 0.01);
    EXPECT_NEAR(player.buffered(), 0.6, 0.01);
    ASSERT_TRUE(reaches(State::Ended, 3000));
    EXPECT_NEAR(player.position(), player.duration(), 1e-9);

    QList<State> seen;
    for (const QList<QVariant> &signal : std::as_const(states)) {
        seen.append(signal.at(0).value<State>());
    }
    EXPECT_EQ(seen, (QList<State>{State::Loading, State::Playing, State::Ended}));
}

TEST_F(PlayerTest, ThePositionAdvancesInRealTime)
{
    player.play(wavSource(synth::sine(440.0, 3.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    const double start = player.position();
    QTest::qWait(400);
    const double advanced = player.position() - start;
    EXPECT_GT(advanced, 0.2);
    EXPECT_LT(advanced, 0.8);
}

TEST_F(PlayerTest, PauseHoldsThePositionAndResumeCarriesOn)
{
    player.play(wavSource(synth::sine(440.0, 3.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    QTest::qWait(150);
    player.pause();
    EXPECT_EQ(player.state(), State::Paused);
    QTest::qWait(50); // the fade-out
    const double paused = player.position();
    QTest::qWait(300);
    EXPECT_NEAR(player.position(), paused, 0.001);

    player.resume();
    EXPECT_EQ(player.state(), State::Playing);
    QTest::qWait(300);
    EXPECT_GT(player.position(), paused + 0.1);
    EXPECT_EQ(player.state(), State::Playing);
}

TEST_F(PlayerTest, ToggleSwitchesBetweenPlayingAndPaused)
{
    player.toggle(); // nothing loaded: nothing happens
    EXPECT_EQ(player.state(), State::Idle);
    player.play(wavSource(synth::sine(440.0, 2.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.toggle();
    EXPECT_EQ(player.state(), State::Paused);
    player.toggle();
    EXPECT_EQ(player.state(), State::Playing);
}

TEST_F(PlayerTest, SeekShowsTheTargetAtOnceAndPlaysFromThere)
{
    player.play(wavSource(synth::sine(440.0, 4.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.seek(3.2);
    EXPECT_NEAR(player.position(), 3.2, 1e-9); // before the decoder has moved
    QElapsedTimer timer;
    timer.start();
    ASSERT_TRUE(reaches(State::Ended, 3000));
    EXPECT_LT(timer.elapsed(), 2000); // 0.8 s were left, not 4
}

TEST_F(PlayerTest, SeekIsClampedToTheTrack)
{
    player.play(wavSource(synth::sine(440.0, 2.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.seek(-5.0);
    EXPECT_NEAR(player.position(), 0.0, 1e-9);
    player.seek(60.0);
    EXPECT_NEAR(player.position(), 2.0, 1e-9);
    ASSERT_TRUE(reaches(State::Ended, 2000));
}

TEST_F(PlayerTest, SeekWhilePausedStaysPaused)
{
    player.play(wavSource(synth::sine(440.0, 3.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.pause();
    player.seek(1.5);
    QTest::qWait(200);
    EXPECT_EQ(player.state(), State::Paused);
    EXPECT_NEAR(player.position(), 1.5, 0.001);
    player.resume();
    QTest::qWait(300);
    EXPECT_GT(player.position(), 1.6);
}

TEST_F(PlayerTest, SkipsAddUpBeforeTheDecoderCatchesUp)
{
    player.play(wavSource(synth::sine(440.0, 10.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.seek(1.0);
    player.skip(2.0);
    player.skip(2.0);
    EXPECT_NEAR(player.position(), 5.0, 1e-9);
    player.skip(-30.0);
    EXPECT_NEAR(player.position(), 0.0, 1e-9);
}

TEST_F(PlayerTest, ResumeAfterTheEndPlaysAgain)
{
    player.play(wavSource(synth::sine(440.0, 0.3, kRate)), track(u"Short"_s));
    ASSERT_TRUE(reaches(State::Ended, 3000));
    player.resume();
    EXPECT_EQ(player.state(), State::Playing);
    EXPECT_LT(player.position(), 0.1);
    ASSERT_TRUE(reaches(State::Ended, 3000));

    // Seeking after the end plays from there.
    player.seek(0.1);
    EXPECT_EQ(player.state(), State::Playing);
    ASSERT_TRUE(reaches(State::Ended, 3000));
}

TEST_F(PlayerTest, StopUnloadsTheTrack)
{
    const QSignalSpy tracks(&player, &Player::trackChanged);
    player.play(wavSource(synth::sine(440.0, 3.0, kRate)), track(u"Long"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.stop();
    EXPECT_EQ(player.state(), State::Idle);
    EXPECT_FALSE(player.isActive());
    EXPECT_EQ(player.position(), 0.0);
    EXPECT_EQ(player.source(), nullptr);
    EXPECT_TRUE(player.track().title.isEmpty());
    EXPECT_EQ(tracks.size(), 2);
    player.seek(1.0); // nothing loaded: ignored
    player.resume();
    EXPECT_EQ(player.state(), State::Idle);
}

TEST_F(PlayerTest, PlayReplacesWhatWasPlaying)
{
    player.play(wavSource(synth::sine(440.0, 5.0, kRate)), track(u"First"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.play(wavSource(synth::sine(880.0, 0.4, kRate)), track(u"Second"_s));
    EXPECT_EQ(player.track().title, u"Second"_s);
    ASSERT_TRUE(reaches(State::Ended, 3000));
    EXPECT_NEAR(player.duration(), 0.4, 0.01);
}

TEST_F(PlayerTest, PlayingTheNextTrackRightAwayNeverShowsTheFirstOnesLength)
{
    QSignalSpy durations(&player, &Player::durationChanged);
    player.play(wavSource(synth::sine(440.0, 5.0, kRate)), track(u"First"_s));
    player.play(wavSource(synth::sine(880.0, 0.5, kRate)), track(u"Second"_s));
    ASSERT_TRUE(reaches(State::Ended, 3000));
    for (const QList<QVariant> &signal : std::as_const(durations)) {
        EXPECT_LT(signal.at(0).toDouble(), 1.0);
    }
}

TEST_F(PlayerTest, AFileThatIsNotAudioFails)
{
    // Bytes the decoder is known to reject (its own garbage test): some random inputs look like MPEG audio.
    player.play(std::make_shared<MemorySource>(synth::randomBytes(500'000, 1234)), track(u"Noise"_s));
    ASSERT_TRUE(reaches(State::Failed));
    EXPECT_FALSE(player.error().isEmpty());
    EXPECT_FALSE(player.isActive());
    player.seek(1.0); // ignored once failed
    EXPECT_EQ(player.state(), State::Failed);

    // The next track plays normally.
    player.play(wavSource(synth::sine(440.0, 0.3, kRate)), track(u"Short"_s));
    EXPECT_TRUE(player.error().isEmpty());
    ASSERT_TRUE(reaches(State::Ended, 3000));
}

TEST_F(PlayerTest, ADownloadThatFallsBehindBuffersAndCarriesOn)
{
    const std::shared_ptr<MemorySource> source = wavSource(synth::sine(440.0, 2.0, kRate), 0.25);
    player.play(source, track(u"Downloading"_s));
    ASSERT_TRUE(reaches(State::Playing));
    EXPECT_NEAR(player.duration(), 2.0, 0.01); // from the header, before the data
    EXPECT_LT(player.buffered(), 0.6);
    ASSERT_TRUE(reaches(State::Buffering, 3000)); // ran dry after about half a second
    const double held = player.position();
    EXPECT_LT(held, 0.6);

    source->setComplete();
    ASSERT_TRUE(reaches(State::Playing));
    EXPECT_NEAR(player.buffered(), 2.0, 0.01);
    ASSERT_TRUE(reaches(State::Ended, 4000));
}

TEST_F(PlayerTest, SeekingPastTheDownloadedPartWaitsForTheData)
{
    const std::shared_ptr<MemorySource> source = wavSource(synth::sine(440.0, 3.0, kRate), 0.3);
    player.play(source, track(u"Downloading"_s));
    ASSERT_TRUE(reaches(State::Playing));
    player.seek(2.5);
    ASSERT_TRUE(reaches(State::Buffering));
    EXPECT_NEAR(player.position(), 2.5, 0.001);
    source->setComplete();
    ASSERT_TRUE(reaches(State::Ended, 3000));
}

TEST_F(PlayerTest, AFailedDownloadStopsPlayback)
{
    const std::shared_ptr<MemorySource> source = wavSource(synth::sine(440.0, 3.0, kRate), 0.1);
    player.play(source, track(u"Downloading"_s));
    ASSERT_TRUE(reaches(State::Playing));
    source->setFailed();
    ASSERT_TRUE(reaches(State::Failed, 3000));
    EXPECT_FALSE(player.error().isEmpty());
}

TEST_F(PlayerTest, AFileThatNeedsTheWholeDownloadSaysSoAndWaits)
{
    // An M4A with its index at the end can't be decoded until it is complete.
    const QByteArray m4a = synth::makeM4a(synth::sine(440.0, 1.0, kRate), {.alac = true, .fastStart = false});
    const auto source = std::make_shared<MemorySource>(m4a, m4a.size() / 2);
    player.play(source, track(u"Apple"_s));
    ASSERT_TRUE(waitFor([this] { return !player.detail().isEmpty(); }));
    EXPECT_EQ(player.state(), State::Loading);
    EXPECT_TRUE(player.detail().contains(u"downloaded"_s)) << player.detail().toStdString();

    source->setComplete();
    ASSERT_TRUE(reaches(State::Playing));
    EXPECT_TRUE(player.detail().isEmpty());
    ASSERT_TRUE(reaches(State::Ended, 3000));
}

TEST_F(PlayerTest, VolumeIsClamped)
{
    EXPECT_EQ(player.volume(), 1.0);
    player.setVolume(0.25);
    EXPECT_EQ(player.volume(), 0.25);
    player.setVolume(3.0);
    EXPECT_EQ(player.volume(), 1.0);
    player.setVolume(-1.0);
    EXPECT_EQ(player.volume(), 0.0);
}

TEST_F(PlayerRenderTest, RendersTheDecodedSignalWithAFadeIn)
{
    player.play(wavSource(synth::sine(1000.0, 2.0, kRate)), track(u"Tone"_s));
    const std::vector<float> first = pullUntilAudible();
    ASSERT_FALSE(first.empty());
    EXPECT_LT(std::abs(first[0]) + std::abs(first[1]), 0.05F); // starts from silence
    std::vector<float> steady = output->pull(4800);
    EXPECT_NEAR(rmsOf(steady), 0.5 / std::sqrt(2.0), 0.02); // the 0.5 sine at full volume
    QTest::qWait(50);
    EXPECT_EQ(player.state(), State::Playing);

    player.setVolume(0.5); // gain 0.25: perceptual curve
    output->pull(480);     // the ramp
    steady = output->pull(4800);
    EXPECT_NEAR(rmsOf(steady), 0.25 * 0.5 / std::sqrt(2.0), 0.01);
}

TEST_F(PlayerRenderTest, PauseFadesOutThenKeepsTheRestForLater)
{
    player.play(wavSource(synth::sine(1000.0, 2.0, kRate)), track(u"Tone"_s));
    ASSERT_FALSE(pullUntilAudible().empty());
    output->pull(2400);
    const double before = player.position();
    player.pause();
    const std::vector<float> fading = output->pull(480);
    EXPECT_GT(peakOf(fading), 0.0F);
    EXPECT_EQ(peakOf(std::vector<float>(fading.begin() + kFadedBy, fading.end())), 0.0F); // silent within ~5 ms
    for (int block = 0; block < 20; ++block) {
        EXPECT_EQ(peakOf(output->pull(480)), 0.0F);
    }
    EXPECT_NEAR(player.position(), before, 300.0 / kRate); // only the fade was consumed

    player.resume();
    EXPECT_GT(peakOf(output->pull(480)), 0.0F); // no new wait: the ring still holds the audio
}

TEST_F(PlayerRenderTest, SeekDropsTheOldAudio)
{
    // A tone, then silence: a seek into the silence must not let the tone play on.
    const synth::Pcm pcm = synth::concatenated(synth::sine(1000.0, 1.0, kRate), synth::silence(1.0, kRate));
    player.play(wavSource(pcm), track(u"Half"_s));
    ASSERT_FALSE(pullUntilAudible().empty());
    output->pull(480);
    player.seek(1.5);
    const std::vector<float> fade = output->pull(480);
    EXPECT_EQ(peakOf(std::vector<float>(fade.begin() + kFadedBy, fade.end())), 0.0F); // faded out, then dropped
    for (int block = 0; block < 50; ++block) {
        EXPECT_EQ(peakOf(output->pull(480)), 0.0F) << "block " << block;
        QTest::qWait(1);
    }

    player.seek(0.2);
    EXPECT_FALSE(pullUntilAudible().empty()); // back in the tone
}

TEST_F(PlayerRenderTest, AnOutputThatWontStartFails)
{
    output->refuse = true;
    player.play(wavSource(synth::sine(440.0, 1.0, kRate)), track(u"Tone"_s));
    EXPECT_EQ(player.state(), State::Failed);
    EXPECT_TRUE(player.error().contains(u"the device is gone"_s));
}

TEST_F(PlayerRenderTest, OutputsSilenceWhenIdle)
{
    const std::vector<float> samples = output->pull(480); // not started: the test's own fill shows through
    EXPECT_EQ(samples.size(), 960U);
    player.play(wavSource(synth::sine(440.0, 1.0, kRate)), track(u"Tone"_s));
    player.stop();
    EXPECT_FALSE(output->isRunning());
}
