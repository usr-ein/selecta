// SPDX-License-Identifier: GPL-3.0-or-later
// C2: the output rules shared by the transcoder and the advisor (rates, word lengths, dither), settings checks and
// labels.
#include "convert/EncodeSettings.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using selecta::audio::ErrorCode;
using selecta::audio::StreamInfo;

namespace {

StreamInfo lossless(int rate, int bits, int channels = 2)
{
    StreamInfo info;
    info.sampleRate = rate;
    info.bitsPerSample = bits;
    info.channels = channels;
    info.lossless = true;
    return info;
}

StreamInfo lossy(int rate)
{
    StreamInfo info;
    info.sampleRate = rate;
    info.channels = 2;
    return info;
}

EncodeSettings format(OutputFormat output)
{
    EncodeSettings settings;
    settings.format = output;
    return settings;
}

} // namespace

TEST(ResolveOutput, Mp3KeepsMpeg1RatesAndTakesEverythingElseTo44k)
{
    const EncodeSettings mp3;
    for (const int rate : {32000, 44100, 48000}) {
        const ResolvedOutput out = resolveOutput(lossless(rate, 16), mp3);
        EXPECT_EQ(out.sampleRate, rate);
        EXPECT_FALSE(out.resample);
        EXPECT_EQ(out.bitDepth, 0);
        EXPECT_FALSE(out.dither); // LAME takes float: never dithered
    }
    for (const int rate : {88200, 96000, 176400, 192000}) {
        const ResolvedOutput out = resolveOutput(lossless(rate, 24), mp3);
        EXPECT_EQ(out.sampleRate, 44100) << rate; // D-4
        EXPECT_TRUE(out.resample);
        EXPECT_FALSE(out.upsample);
    }
    const ResolvedOutput low = resolveOutput(lossy(22050), mp3);
    EXPECT_EQ(low.sampleRate, 44100);
    EXPECT_TRUE(low.upsample); // flagged for the advisor
    EncodeSettings explicit48 = mp3;
    explicit48.sampleRate = 48000;
    EXPECT_EQ(resolveOutput(lossless(96000, 24), explicit48).sampleRate, 48000);
}

TEST(ResolveOutput, LosslessKeepsRateAndDepthUpToTheCeiling)
{
    for (const OutputFormat output : {OutputFormat::Aiff, OutputFormat::Flac, OutputFormat::Wav, OutputFormat::Alac}) {
        const EncodeSettings settings = format(output);
        const ResolvedOutput cd = resolveOutput(lossless(44100, 16), settings);
        EXPECT_EQ(cd.sampleRate, 44100);
        EXPECT_EQ(cd.bitDepth, 16);
        EXPECT_FALSE(cd.resample);
        EXPECT_FALSE(cd.dither);
        const ResolvedOutput hiRes = resolveOutput(lossless(96000, 24), settings);
        EXPECT_EQ(hiRes.sampleRate, 96000);
        EXPECT_EQ(hiRes.bitDepth, 24);
        EXPECT_FALSE(hiRes.resample);
    }
    const EncodeSettings aiff = format(OutputFormat::Aiff);
    // Above the 96 kHz ceiling: the highest rate of the same family.
    EXPECT_EQ(resolveOutput(lossless(192000, 24), aiff).sampleRate, 96000);
    EXPECT_EQ(resolveOutput(lossless(176400, 24), aiff).sampleRate, 88200);
    EXPECT_EQ(resolveOutput(lossless(352800, 24), aiff).sampleRate, 88200);
    EncodeSettings older = aiff;
    older.maxSampleRate = 48000; // XDJ-class players
    EXPECT_EQ(resolveOutput(lossless(96000, 24), older).sampleRate, 48000);
    EXPECT_EQ(resolveOutput(lossless(88200, 24), older).sampleRate, 44100);
    EncodeSettings cdOnly = aiff;
    cdOnly.maxSampleRate = 44100;
    EXPECT_EQ(resolveOutput(lossless(96000, 24), cdOnly).sampleRate, 44100);
    EXPECT_TRUE(resolveOutput(lossless(192000, 24), aiff).resample);
}

TEST(ResolveOutput, WordLengths)
{
    const EncodeSettings flac = format(OutputFormat::Flac);
    EXPECT_EQ(resolveOutput(lossless(44100, 8), flac).bitDepth, 16);
    EXPECT_EQ(resolveOutput(lossless(44100, 20), flac).bitDepth, 24);
    EXPECT_EQ(resolveOutput(lossless(44100, 32), flac).bitDepth, 24);
    StreamInfo floating = lossless(44100, 32);
    floating.floatingPoint = true;
    EXPECT_EQ(resolveOutput(floating, flac).bitDepth, 24);
    EXPECT_FALSE(resolveOutput(floating, flac).dither); // 24-bit output: rounded, not dithered
    EXPECT_EQ(resolveOutput(lossy(44100), flac).bitDepth, 16);
    EXPECT_TRUE(resolveOutput(lossy(44100), flac).dither);           // decoded lossy audio is off the 16-bit grid
    EXPECT_EQ(resolveOutput(lossless(44100, 0), flac).bitDepth, 24); // unknown depth: nothing truncated
}

TEST(ResolveOutput, DitherOnlyWhenLeavingThe16BitGrid)
{
    EncodeSettings to16 = format(OutputFormat::Aiff);
    to16.bitDepth = 16;
    EXPECT_TRUE(resolveOutput(lossless(44100, 24), to16).dither);
    EXPECT_FALSE(resolveOutput(lossless(44100, 16), to16).dither);
    EXPECT_FALSE(resolveOutput(lossless(44100, 8), to16).dither);
    EXPECT_TRUE(resolveOutput(lossless(192000, 16), to16).dither);   // resampled values are off the grid
    EXPECT_TRUE(resolveOutput(lossless(44100, 16, 6), to16).dither); // a downmix is too
    EncodeSettings to24 = format(OutputFormat::Aiff);
    to24.bitDepth = 24;
    const ResolvedOutput up = resolveOutput(lossless(44100, 16), to24);
    EXPECT_EQ(up.bitDepth, 24);
    EXPECT_TRUE(up.upDepth); // W-UPDEPTH
    EXPECT_FALSE(up.dither);
    EXPECT_FALSE(resolveOutput(lossy(44100), to24).upDepth); // a lossy source has no word length to pad
}

TEST(ResolveOutput, ExplicitUpsamplingIsFlagged)
{
    EncodeSettings settings = format(OutputFormat::Flac);
    settings.sampleRate = 96000;
    const ResolvedOutput out = resolveOutput(lossless(44100, 16), settings);
    EXPECT_EQ(out.sampleRate, 96000);
    EXPECT_TRUE(out.resample);
    EXPECT_TRUE(out.upsample); // W-UPSAMPLE
}

TEST(ResolveOutput, KeepAudioChangesNothing)
{
    const EncodeSettings keep = format(OutputFormat::KeepAudio);
    const ResolvedOutput out = resolveOutput(lossless(192000, 24), keep);
    EXPECT_EQ(out.sampleRate, 192000);
    EXPECT_EQ(out.bitDepth, 24);
    EXPECT_FALSE(out.resample || out.dither || out.upsample || out.upDepth);
    EXPECT_EQ(resolveOutput(lossy(44100), keep).bitDepth, 0);
}

TEST(EncodeSettings, CheckSettings)
{
    EXPECT_TRUE(checkSettings({}).ok());
    const auto refused = [](const EncodeSettings &settings) {
        const auto result = checkSettings(settings);
        return !result.ok() && result.error().code == ErrorCode::InvalidArgument;
    };
    EncodeSettings settings;
    settings.mp3.bitrateKbps = 300;
    EXPECT_TRUE(refused(settings));
    settings = {};
    settings.mp3.mode = Mp3Mode::Vbr;
    settings.mp3.vbrQuality = 10;
    EXPECT_TRUE(refused(settings));
    settings = {};
    settings.mp3.algorithmQuality = -1;
    EXPECT_TRUE(refused(settings));
    settings = {};
    settings.sampleRate = 22050; // not MPEG-1 Layer III
    EXPECT_TRUE(refused(settings));
    settings = format(OutputFormat::Flac);
    settings.sampleRate = 22050;
    EXPECT_TRUE(checkSettings(settings).ok());
    settings.sampleRate = 4000;
    EXPECT_TRUE(refused(settings));
    settings = format(OutputFormat::Wav);
    settings.bitDepth = 20;
    EXPECT_TRUE(refused(settings));
    settings = format(OutputFormat::Aiff);
    settings.maxSampleRate = 32000;
    EXPECT_TRUE(refused(settings));
    settings.format = OutputFormat::Mp3;
    settings.maxSampleRate = 96000;
    settings.mp3.mode = Mp3Mode::Vbr; // the CBR bitrate doesn't matter for VBR
    settings.mp3.bitrateKbps = 1;
    EXPECT_TRUE(checkSettings(settings).ok());
}

TEST(EncodeSettings, Labels)
{
    EXPECT_EQ(describeSettings({}), u"MP3 320 kbps CBR"_s);
    EncodeSettings vbr;
    vbr.mp3.mode = Mp3Mode::Vbr;
    vbr.mp3.vbrQuality = 2;
    EXPECT_EQ(describeSettings(vbr), u"MP3 V2"_s);
    EncodeSettings aiff = format(OutputFormat::Aiff);
    EXPECT_EQ(describeSettings(aiff), u"AIFF"_s);
    aiff.bitDepth = 16;
    aiff.sampleRate = 44100;
    EXPECT_EQ(describeSettings(aiff), u"AIFF, 16-bit, 44.1 kHz"_s);
    EncodeSettings flac = format(OutputFormat::Flac);
    flac.sampleRate = 48000;
    EXPECT_EQ(describeSettings(flac), u"FLAC, 48 kHz"_s);
    EXPECT_EQ(describeSettings(format(OutputFormat::Alac)), u"ALAC"_s);
    EXPECT_EQ(describeSettings(format(OutputFormat::KeepAudio)), u"Original audio"_s);

    EXPECT_EQ(fileExtension(OutputFormat::Mp3), u"mp3"_s);
    EXPECT_EQ(fileExtension(OutputFormat::Aiff), u"aiff"_s);
    EXPECT_EQ(fileExtension(OutputFormat::Flac), u"flac"_s);
    EXPECT_EQ(fileExtension(OutputFormat::Wav), u"wav"_s);
    EXPECT_EQ(fileExtension(OutputFormat::Alac), u"m4a"_s);
    EXPECT_TRUE(fileExtension(OutputFormat::KeepAudio).isEmpty());

    EXPECT_EQ(lameSettingsString({}), u"LAME3.100 -b 320"_s);
    Mp3Settings v0;
    v0.mode = Mp3Mode::Vbr;
    EXPECT_EQ(lameSettingsString(v0), u"LAME3.100 -V 0"_s);
    Mp3Settings fast;
    fast.bitrateKbps = 256;
    fast.algorithmQuality = 7;
    EXPECT_EQ(lameSettingsString(fast), u"LAME3.100 -b 256 -q 7"_s);
}
