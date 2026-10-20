// SPDX-License-Identifier: GPL-3.0-or-later
// A3: quality checks on synthetic sources: spectral cut-off against LAME's lowpass, upconverted and re-encoded MP3s,
// lossless files made from MP3s, padded 24-bit and upsampled files, and the cases that can't be judged.
#include "audio/QualityCheck.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::audio;
using namespace selecta::testing;

namespace {

Result<QualityReport> check(const QByteArray &file)
{
    return analyzeQuality(std::make_shared<MemorySource>(file));
}

QualityReport report(const QByteArray &file)
{
    const Result<QualityReport> result = check(file);
    if (!result.ok()) {
        ADD_FAILURE() << result.error().toString().toStdString();
        return {};
    }
    return result.value();
}

// Broadband material: noise plus a few tones, so every band has content up to Nyquist.
Pcm broadband(double seconds, int sampleRate = 44100, int channels = 2)
{
    Pcm signal = tones({110, 1000, 7000}, seconds, sampleRate, channels, 0.08);
    const Pcm noise = whiteNoise(seconds, 99, sampleRate, channels, 0.25);
    for (std::size_t i = 0; i < signal.samples.size(); ++i) {
        signal.samples[i] += noise.samples[i];
    }
    return signal;
}

const Pcm &cdSource()
{
    static const Pcm pcm = quantised(broadband(8.0), 16);
    return pcm;
}

Pcm decoded(const QByteArray &file, int sampleRate = 0)
{
    DecoderOptions options;
    options.channels = 0;
    options.sampleRate = sampleRate;
    options.resampler = ResamplerQuality::VeryHigh;
    return decodeAll(file, options).pcm;
}

std::string findings(const QualityReport &r)
{
    std::string text;
    for (const QualityFinding finding : r.findings) {
        text += describeFinding(finding, r).toStdString() + "; ";
    }
    return text;
}

} // namespace

TEST(QualityCheck, GenuineLosslessHasNoFindings)
{
    const QualityReport cd = report(makeWav(cdSource()));
    EXPECT_GE(cd.cutoffHz, 21500); // full band, to Nyquist
    EXPECT_TRUE(cd.findings.isEmpty()) << findings(cd);
    EXPECT_EQ(cd.sourceBitDepth, 16);
    EXPECT_EQ(cd.effectiveBitDepth, 16);
    EXPECT_EQ(cd.sampleRate, 44100);
    EXPECT_EQ(cd.contentBandwidthHz, -1); // only measured above 48 kHz
    EXPECT_FALSE(cd.likelySourceKbps.has_value());

    const QualityReport at48 = report(makeFlac(quantised(broadband(4.0, 48000), 24), {.bits = 24}));
    EXPECT_GE(at48.cutoffHz, 23500);
    EXPECT_EQ(at48.effectiveBitDepth, 24);
    EXPECT_TRUE(at48.findings.isEmpty()) << findings(at48);
}

TEST(QualityCheck, TheCutoffFollowsLamesLowpass)
{
    const std::array<std::pair<int, int>, 4> lowpass = {{{128, 17000}, {160, 17500}, {192, 18600}, {320, 20500}}};
    for (const auto &[kbps, hz] : lowpass) {
        const QualityReport mp3 = report(makeMp3(cdSource(), {.bitrateKbps = kbps}));
        EXPECT_NEAR(mp3.cutoffHz, hz, 600) << kbps;
        EXPECT_EQ(mp3.lameLowpassHz, hz) << kbps;
        EXPECT_TRUE(mp3.findings.isEmpty()) << kbps << ": " << findings(mp3); // genuine at its own bitrate
        EXPECT_EQ(mp3.sourceBitDepth, 0);
    }
    const QualityReport v0 = report(makeMp3(cdSource(), {.vbr = true, .vbrQuality = 0}));
    EXPECT_GE(v0.cutoffHz, 21500); // V0 has no lowpass at 44.1 kHz
    EXPECT_TRUE(v0.findings.isEmpty()) << findings(v0);
}

TEST(QualityCheck, LosslessMadeFromAnMp3IsLikelyFromLossy)
{
    for (const auto &[kbps, likely] : std::array<std::pair<int, int>, 3>{{{128, 128}, {160, 160}, {192, 192}}}) {
        const Pcm fromMp3 = quantised(decoded(makeMp3(cdSource(), {.bitrateKbps = kbps})), 16);
        const QualityReport flac = report(makeFlac(fromMp3));
        ASSERT_TRUE(flac.hasFinding(QualityFinding::LikelyFromLossy)) << kbps << ": cut-off " << flac.cutoffHz;
        EXPECT_EQ(flac.likelySourceKbps, likely);
        const QString text = describeFinding(QualityFinding::LikelyFromLossy, flac);
        EXPECT_TRUE(text.startsWith(u"Likely from a lossy source (≈ %1 kbps MP3"_s.arg(likely))) << text.toStdString();
    }
    // A 320 kbps or V0 source keeps (almost) the whole band: not flagged, a known limit of every cut-off detector.
    for (const Mp3Options options : {Mp3Options{.bitrateKbps = 320}, Mp3Options{.vbr = true, .vbrQuality = 0}}) {
        const QualityReport flac = report(makeFlac(quantised(decoded(makeMp3(cdSource(), options)), 16)));
        EXPECT_FALSE(flac.hasFinding(QualityFinding::LikelyFromLossy)) << flac.cutoffHz;
    }
}

TEST(QualityCheck, UpconvertedAndReencodedMp3s)
{
    // A "320" made from a 128 kbps MP3: the cut-off of the 128 survives, and the new LAME tag claims 20.5 kHz.
    const Pcm from128 = decoded(makeMp3(cdSource(), {.bitrateKbps = 128}));
    const QualityReport fake320 = report(makeMp3(from128, {.bitrateKbps = 320}));
    EXPECT_TRUE(fake320.hasFinding(QualityFinding::LikelyUpconvertedMp3)) << fake320.cutoffHz;
    EXPECT_TRUE(fake320.hasFinding(QualityFinding::LikelyReencoded));
    EXPECT_EQ(fake320.likelySourceKbps, 128);
    EXPECT_TRUE(describeFinding(QualityFinding::LikelyUpconvertedMp3, fake320).startsWith(u"Likely upconverted"_s));
    EXPECT_TRUE(describeFinding(QualityFinding::LikelyReencoded, fake320).contains(u"20.5 kHz"_s));

    // From 192: above the upconversion limit, but well below what its own LAME tag says.
    const Pcm from192 = decoded(makeMp3(cdSource(), {.bitrateKbps = 192}));
    const QualityReport reencoded = report(makeMp3(from192, {.bitrateKbps = 320}));
    EXPECT_FALSE(reencoded.hasFinding(QualityFinding::LikelyUpconvertedMp3)) << reencoded.cutoffHz;
    EXPECT_TRUE(reencoded.hasFinding(QualityFinding::LikelyReencoded)) << reencoded.cutoffHz;

    // An MP3 below 256 kbps is never "upconverted"; without a LAME tag there is nothing to cross-check.
    const QualityReport plain = report(makeMp3(from128, {.bitrateKbps = 320, .lameTag = false}));
    EXPECT_TRUE(plain.hasFinding(QualityFinding::LikelyUpconvertedMp3));
    EXPECT_FALSE(plain.hasFinding(QualityFinding::LikelyReencoded));
    EXPECT_FALSE(plain.lameLowpassHz.has_value());
}

TEST(QualityCheck, Padded24BitIsExact)
{
    const Pcm sixteen = quantised(broadband(3.0), 16);
    const QualityReport padded = report(makeFlac(sixteen, {.bits = 24}));
    EXPECT_EQ(padded.sourceBitDepth, 24);
    EXPECT_EQ(padded.effectiveBitDepth, 16);
    EXPECT_TRUE(padded.hasFinding(QualityFinding::LikelyPadded24Bit));
    EXPECT_EQ(describeFinding(QualityFinding::LikelyPadded24Bit, padded), u"Likely 16-bit audio padded to 24 bits"_s);
    EXPECT_TRUE(report(makeWav(sixteen, {.bits = 24})).hasFinding(QualityFinding::LikelyPadded24Bit));
    const QualityReport floating = report(makeWav(sixteen, {.bits = 32, .floatingPoint = true}));
    EXPECT_TRUE(floating.hasFinding(QualityFinding::LikelyPadded24Bit));
    EXPECT_EQ(floating.effectiveBitDepth, 16);

    // One sample off the 16-bit grid is enough to be genuine.
    Pcm almost = sixteen;
    almost.samples[almost.samples.size() / 2] += static_cast<float>(1.0 / 8388608.0);
    const QualityReport genuine = report(makeFlac(almost, {.bits = 24}));
    EXPECT_EQ(genuine.effectiveBitDepth, 24);
    EXPECT_FALSE(genuine.hasFinding(QualityFinding::LikelyPadded24Bit));
    // Digital silence proves nothing.
    EXPECT_FALSE(report(makeFlac(silence(3.0), {.bits = 24})).hasFinding(QualityFinding::LikelyPadded24Bit));
    // 16-bit files aren't checked.
    EXPECT_FALSE(report(makeFlac(sixteen)).hasFinding(QualityFinding::LikelyPadded24Bit));
}

TEST(QualityCheck, UpsampledFilesHaveNothingAboveTheCdBand)
{
    const QualityReport genuine = report(makeFlac(quantised(broadband(5.0, 96000), 24), {.bits = 24}));
    EXPECT_GE(genuine.contentBandwidthHz, 45000);
    EXPECT_TRUE(genuine.findings.isEmpty()) << findings(genuine);

    const Pcm upsampled = quantised(decoded(makeWav(cdSource()), 96000), 24);
    const QualityReport fake = report(makeFlac(upsampled, {.bits = 24}));
    EXPECT_TRUE(fake.hasFinding(QualityFinding::LikelyUpsampled)) << fake.contentBandwidthHz;
    EXPECT_FALSE(fake.hasFinding(QualityFinding::LikelyFromLossy)) << fake.cutoffHz;
    EXPECT_LE(fake.contentBandwidthHz, 24000);
    EXPECT_TRUE(describeFinding(QualityFinding::LikelyUpsampled, fake).contains(u"96.0 kHz"_s));

    // Hi-res made from an MP3: both.
    const Pcm fromMp3 = quantised(decoded(makeMp3(cdSource(), {.bitrateKbps = 192}), 96000), 24);
    const QualityReport worse = report(makeFlac(fromMp3, {.bits = 24}));
    EXPECT_TRUE(worse.hasFinding(QualityFinding::LikelyUpsampled));
    EXPECT_TRUE(worse.hasFinding(QualityFinding::LikelyFromLossy)) << worse.cutoffHz;
}

TEST(QualityCheck, WhatCantBeJudgedIsntJudged)
{
    const QualityReport shortFile = report(makeWav(quantised(broadband(1.5), 16)));
    EXPECT_EQ(shortFile.cutoffHz, -1);
    EXPECT_TRUE(shortFile.findings.isEmpty());
    const QualityReport quiet = report(makeFlac(silence(4.0)));
    EXPECT_EQ(quiet.cutoffHz, -1);
    EXPECT_TRUE(quiet.findings.isEmpty());
    const QualityReport lowRate = report(makeWav(quantised(broadband(4.0, 22050), 16)));
    EXPECT_EQ(lowRate.cutoffHz, -1);
    // Pure tones with nothing but the quantisation noise floor above them: no reference band to measure against.
    const QualityReport dark = report(makeWav(quantised(tones({100, 440, 3000}, 4.0, 44100, 2, 0.2), 16)));
    EXPECT_EQ(dark.cutoffHz, -1);
    EXPECT_TRUE(dark.findings.isEmpty());
    // Mono works like stereo.
    const QualityReport mono = report(makeWav(quantised(broadband(4.0, 44100, 1), 16)));
    EXPECT_GE(mono.cutoffHz, 21500);
}

TEST(QualityCheck, Failures)
{
    const QByteArray wav = makeWav(cdSource());
    Result<QualityReport> result = analyzeQuality(std::make_shared<MemorySource>(wav, wav.size() / 2));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument); // a download in progress
    result = analyzeQuality(nullptr);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    result = check(randomBytes(64 * 1024, 5));
    EXPECT_FALSE(result.ok());
    CancellationFlag cancel;
    cancel.cancel();
    result = analyzeQuality(std::make_shared<MemorySource>(wav), {}, &cancel);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
}

TEST(QualityCheck, FindingNamesAndBadges)
{
    // The names are stored (Inbox items keep them): they never change.
    EXPECT_EQ(findingName(QualityFinding::LikelyFromLossy), u"from-lossy"_s);
    EXPECT_EQ(findingName(QualityFinding::LikelyUpconvertedMp3), u"upconverted-mp3"_s);
    EXPECT_EQ(findingName(QualityFinding::LikelyReencoded), u"reencoded"_s);
    EXPECT_EQ(findingName(QualityFinding::LikelyPadded24Bit), u"padded-24-bit"_s);
    EXPECT_EQ(findingName(QualityFinding::LikelyUpsampled), u"upsampled"_s);
    for (const QualityFinding finding :
         {QualityFinding::LikelyFromLossy, QualityFinding::LikelyUpconvertedMp3, QualityFinding::LikelyReencoded,
          QualityFinding::LikelyPadded24Bit, QualityFinding::LikelyUpsampled}) {
        EXPECT_EQ(findingFromName(findingName(finding)), finding);
        for (const QString &badge : {findingBadge(finding), findingBadge(finding, 192)}) {
            EXPECT_TRUE(badge.startsWith(u"Likely "_s)) << badge.toStdString();
            EXPECT_FALSE(badge.contains(u"fake"_s, Qt::CaseInsensitive)) << badge.toStdString();
        }
    }
    for (const QString &unknown : {u""_s, u"From-Lossy"_s, u" from-lossy"_s, u"fake"_s, u"from-lossy,upsampled"_s}) {
        EXPECT_FALSE(findingFromName(unknown).has_value()) << unknown.toStdString();
    }

    EXPECT_EQ(findingBadge(QualityFinding::LikelyFromLossy, 192), u"Likely from a ≈ 192 kbps MP3"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyFromLossy), u"Likely from a lossy file"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyUpconvertedMp3, 128), u"Likely upconverted from ≈ 128 kbps"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyUpconvertedMp3), u"Likely upconverted from a lower bitrate"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyReencoded, 160), u"Likely re-encoded from a lossy file"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyPadded24Bit), u"Likely 16-bit, padded"_s);
    EXPECT_EQ(findingBadge(QualityFinding::LikelyUpsampled), u"Likely upsampled"_s);
}
