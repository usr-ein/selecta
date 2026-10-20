// SPDX-License-Identifier: GPL-3.0-or-later
// C3: the quality advisor's decision table (conversion §6.2), its notices and their copy (§6.3), the per-file verdict
// for the inspector, and probeSourceQuality() on real files.
#include "convert/QualityAdvisor.h"

#include "ConvertTestUtil.h"
#include "DecodeHelpers.h"
#include "SyntheticAudio.h"

#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;
using selecta::audio::Codec;
using selecta::audio::Container;
using selecta::audio::DecoderOptions;
using selecta::audio::ErrorCode;
using selecta::audio::QualityFinding;
using selecta::audio::QualityReport;

namespace {

SourceQuality lossless(int bits, int rate, Container container = Container::Flac, Codec codec = Codec::Flac)
{
    SourceQuality source;
    source.container = container;
    source.codec = codec;
    source.lossless = true;
    source.bitDepth = bits;
    source.effectiveBitDepth = bits;
    source.sampleRate = rate;
    source.channels = 2;
    source.bitrateKbps = static_cast<int>((qint64{rate} * 2 * bits * 6) / 10000); // FLAC at ~60 % of PCM
    return source;
}

SourceQuality lossy(Codec codec, int kbps, bool cbr = true, Container container = Container::MpegAudio)
{
    SourceQuality source;
    source.container = container;
    source.codec = codec;
    source.sampleRate = 44100;
    source.channels = 2;
    source.bitrateKbps = kbps;
    source.constantBitrate = cbr;
    return source;
}

SourceQuality withFinding(SourceQuality source, QualityFinding finding, int cutoffHz = -1,
                          std::optional<int> likelyKbps = std::nullopt)
{
    QualityReport report = source.checks.value_or(QualityReport{});
    report.sampleRate = source.sampleRate;
    report.sourceBitDepth = source.bitDepth;
    report.effectiveBitDepth = finding == QualityFinding::LikelyPadded24Bit ? 16 : source.bitDepth;
    report.cutoffHz = cutoffHz;
    report.likelySourceKbps = likelyKbps;
    report.findings.append(finding);
    source.effectiveBitDepth = report.effectiveBitDepth;
    source.checks = report;
    return source;
}

EncodeSettings mp3(int kbps = 320)
{
    EncodeSettings settings;
    settings.mp3.bitrateKbps = kbps;
    return settings;
}

EncodeSettings mp3At(int rate, int kbps = 320)
{
    EncodeSettings settings = mp3(kbps);
    settings.sampleRate = rate;
    return settings;
}

SourceQuality at(SourceQuality source, int rate)
{
    source.sampleRate = rate;
    return source;
}

EncodeSettings vbr(int quality)
{
    EncodeSettings settings;
    settings.mp3.mode = Mp3Mode::Vbr;
    settings.mp3.vbrQuality = quality;
    return settings;
}

EncodeSettings output(OutputFormat format, int bits = 0, int rate = 0)
{
    EncodeSettings settings;
    settings.format = format;
    settings.bitDepth = bits;
    settings.sampleRate = rate;
    return settings;
}

Advice filing(const SourceQuality &source, const EncodeSettings &target)
{
    return advise(source, target, AdviceContext::Filing);
}

Advice manual(const SourceQuality &source, const EncodeSettings &target)
{
    return advise(source, target, AdviceContext::Manual);
}

} // namespace

TEST(Advisor, GenuineLossless)
{
    const SourceQuality cd = lossless(16, 44100);
    for (const EncodeSettings &target : {mp3(), vbr(0), output(OutputFormat::Aiff), output(OutputFormat::Aiff, 16),
                                         output(OutputFormat::Flac), output(OutputFormat::Alac)}) {
        const Advice advice = filing(cd, target);
        EXPECT_EQ(advice.sourceClass, SourceClass::LosslessGenuine);
        EXPECT_EQ(advice.action, Action::Convert) << describeSettings(target).toStdString();
        EXPECT_TRUE(advice.notices.isEmpty()) << describeSettings(target).toStdString();
        EXPECT_EQ(advice.badge, Badge::None);
    }
    const Advice up = manual(cd, output(OutputFormat::Aiff, 24));
    EXPECT_EQ(up.action, Action::Ask);
    EXPECT_EQ(up.notices, QList<Notice>{Notice::UpDepth});
    const Advice upRate = manual(cd, output(OutputFormat::Flac, 0, 96000));
    EXPECT_EQ(upRate.action, Action::Ask);
    EXPECT_EQ(upRate.notices, QList<Notice>{Notice::Upsample});
    const Advice wav = filing(cd, output(OutputFormat::Wav));
    EXPECT_EQ(wav.action, Action::Convert);
    EXPECT_EQ(wav.notices, QList<Notice>{Notice::WavTags});
    const Advice down = manual(lossless(16, 48000), output(OutputFormat::Aiff, 0, 44100));
    EXPECT_EQ(down.action, Action::Convert);
    EXPECT_EQ(down.notices, (QList<Notice>{Notice::Resample, Notice::Dither}));
    EXPECT_EQ(down.outputSampleRate, 44100);
    EXPECT_EQ(down.outputBitDepth, 16);
}

TEST(Advisor, HiResLossless)
{
    const SourceQuality hiRes = lossless(24, 96000);
    const Advice toMp3 = filing(hiRes, mp3());
    EXPECT_EQ(toMp3.sourceClass, SourceClass::LosslessHiRes);
    EXPECT_EQ(toMp3.action, Action::Convert);
    EXPECT_EQ(toMp3.notices, QList<Notice>{Notice::Resample}); // → 44.1 kHz (D-4)
    EXPECT_EQ(toMp3.outputSampleRate, 44100);
    EXPECT_EQ(toMp3.outputBitDepth, 0);
    EXPECT_TRUE(filing(lossless(24, 48000), mp3()).notices.isEmpty()); // 48 kHz is an MP3 rate

    const Advice same = filing(hiRes, output(OutputFormat::Aiff));
    EXPECT_EQ(same.action, Action::Convert);
    EXPECT_TRUE(same.notices.isEmpty());
    const Advice capped = filing(lossless(24, 192000), output(OutputFormat::Aiff));
    EXPECT_EQ(capped.notices, QList<Notice>{Notice::Resample}); // 192 → 96 kHz (D-3)
    EXPECT_EQ(capped.outputSampleRate, 96000);
    const Advice to16 = manual(hiRes, output(OutputFormat::Aiff, 16));
    EXPECT_EQ(to16.notices, QList<Notice>{Notice::Dither});

    // A file the players can't play: explicit 192 kHz, or a hardware profile below the target's ceiling.
    EncodeSettings full = output(OutputFormat::Aiff);
    full.maxSampleRate = 192000;
    HardwareProfile older;
    older.name = u"XDJ-1000MK2"_s;
    older.maxSampleRate = 48000;
    const Advice tooHigh = advise(lossless(24, 192000), full, AdviceContext::Manual, older);
    EXPECT_EQ(tooHigh.action, Action::Ask);
    EXPECT_TRUE(tooHigh.hasNotice(Notice::HardwareRate));
    const NoticeText text = describeNotice(Notice::HardwareRate, lossless(24, 192000), full, tooHigh, older);
    EXPECT_EQ(text.detail, u"192 kHz / 24-bit exceeds XDJ-1000MK2 (max 48 kHz / 24-bit)."_s);
}

TEST(Advisor, SuspectLossless)
{
    const SourceQuality fromMp3 = withFinding(lossless(16, 44100), QualityFinding::LikelyFromLossy, 18831, 192);
    const Advice toMp3 = filing(fromMp3, mp3());
    EXPECT_EQ(toMp3.sourceClass, SourceClass::LosslessSuspect);
    EXPECT_EQ(toMp3.action, Action::Ask); // a second-generation lossy file
    EXPECT_EQ(toMp3.notices, QList<Notice>{Notice::Suspect});
    const NoticeText suspect = describeNotice(Notice::Suspect, fromMp3, mp3(), toMp3);
    EXPECT_EQ(suspect.shortText, u"Source is probably not what it claims"_s);
    EXPECT_TRUE(suspect.detail.contains(u"18.8 kHz, typical of a 192 kbps MP3"_s)) << suspect.detail.toStdString();

    const Advice toAiff = filing(fromMp3, output(OutputFormat::Aiff));
    EXPECT_EQ(toAiff.action, Action::Convert); // harmless
    EXPECT_EQ(toAiff.notices, QList<Notice>{Notice::Suspect});

    // Padded 24-bit reduced to 16 bits: lossless, said so instead of "dither".
    const SourceQuality padded = withFinding(lossless(24, 44100), QualityFinding::LikelyPadded24Bit);
    const Advice reduced = manual(padded, output(OutputFormat::Aiff, 16));
    EXPECT_EQ(reduced.action, Action::Convert);
    EXPECT_EQ(reduced.notices, (QList<Notice>{Notice::Suspect, Notice::LosslessReduction}));
    EXPECT_EQ(describeNotice(Notice::Suspect, padded, output(OutputFormat::Aiff, 16), reduced).detail,
              u"This 24-bit file contains only 16 bits of real data."_s);
    const Advice raised = manual(fromMp3, output(OutputFormat::Aiff, 24, 96000));
    EXPECT_EQ(raised.action, Action::Ask);
    EXPECT_EQ(raised.notices, (QList<Notice>{Notice::Upsample, Notice::UpDepth}));
}

TEST(Advisor, Mp3Sources)
{
    const SourceQuality top = lossy(Codec::Mp3, 320);
    EXPECT_EQ(filing(top, mp3()).sourceClass, SourceClass::Mp3High);
    EXPECT_EQ(filing(top, mp3()).action, Action::KeepAsIs); // CONV-2: not re-encoded
    EXPECT_EQ(filing(top, mp3()).badge, Badge::None);
    EXPECT_EQ(manual(top, mp3()).action, Action::KeepAsIs);
    EXPECT_EQ(manual(top, vbr(2)).action, Action::Ask);
    EXPECT_EQ(manual(top, vbr(2)).notices, QList<Notice>{Notice::Generation});
    const Advice toAiff = manual(top, output(OutputFormat::Aiff));
    EXPECT_EQ(toAiff.action, Action::Ask);
    EXPECT_EQ(toAiff.notices, QList<Notice>{Notice::LossyToLossless});
    EXPECT_NEAR(toAiff.sizeFactor, 1411.2 / 320.0, 0.01);
    EXPECT_EQ(describeNotice(Notice::LossyToLossless, top, output(OutputFormat::Aiff), toAiff).shortText,
              u"No quality gain — about 4.4× larger"_s);
    EXPECT_EQ(manual(top, output(OutputFormat::Aiff, 24)).action, Action::Refuse); // ⛔
    EXPECT_EQ(filing(top, output(OutputFormat::Aiff)).action, Action::KeepAsIs);
    EXPECT_EQ(filing(top, output(OutputFormat::Aiff)).badge, Badge::FormatDiffers);
    EXPECT_EQ(filing(lossy(Codec::Mp3, 245, false), mp3()).sourceClass, SourceClass::Mp3High); // V0 class

    const SourceQuality lower = lossy(Codec::Mp3, 192);
    const Advice below = filing(lower, mp3());
    EXPECT_EQ(below.sourceClass, SourceClass::Mp3Lower);
    EXPECT_EQ(below.action, Action::KeepAsIs);
    EXPECT_EQ(below.badge, Badge::BelowTarget);
    EXPECT_TRUE(below.notices.isEmpty());
    EXPECT_EQ(manual(lower, mp3()).notices, QList<Notice>{Notice::LossyUp});
    EXPECT_EQ(manual(lower, mp3(128)).notices, QList<Notice>{Notice::Generation});
    EXPECT_EQ(filing(lower, output(OutputFormat::Aiff)).badge, Badge::BelowTarget);
    EXPECT_EQ(filing(lower, vbr(2)).badge, Badge::None); // 192 kbps meets a V2 target

    // A "320" upconverted from 128 is below target, whatever its header says.
    const SourceQuality fake = withFinding(lossy(Codec::Mp3, 320), QualityFinding::LikelyUpconvertedMp3, 16742, 128);
    EXPECT_EQ(filing(fake, mp3()).sourceClass, SourceClass::Mp3Lower);
    EXPECT_EQ(filing(fake, mp3()).badge, Badge::BelowTarget);
    const Advice lossyUp = manual(fake, mp3());
    EXPECT_EQ(lossyUp.notices, QList<Notice>{Notice::LossyUp});
    EXPECT_TRUE(describeNotice(Notice::LossyUp, fake, mp3(), lossyUp).detail.startsWith(u"MP3 at 128 kbps cannot"_s));
}

TEST(Advisor, AacAndUnplayableLossy)
{
    const SourceQuality aac = lossy(Codec::Aac, 256, false, Container::Mp4);
    for (const EncodeSettings &target : {mp3(), output(OutputFormat::Aiff)}) {
        const Advice advice = filing(aac, target);
        EXPECT_EQ(advice.sourceClass, SourceClass::Aac);
        EXPECT_EQ(advice.action, Action::KeepAsIs); // D-9: rekordbox plays it
        EXPECT_EQ(advice.badge, Badge::FormatDiffers);
    }
    EXPECT_EQ(manual(aac, mp3()).notices, QList<Notice>{Notice::Generation});
    EXPECT_EQ(manual(aac, output(OutputFormat::Flac)).notices, QList<Notice>{Notice::LossyToLossless});
    EXPECT_EQ(manual(aac, output(OutputFormat::Flac, 24)).action, Action::Refuse);

    const SourceQuality vorbis = lossy(Codec::Vorbis, 192, false, Container::Ogg);
    const Advice toMp3 = filing(vorbis, mp3());
    EXPECT_EQ(toMp3.sourceClass, SourceClass::LossyUnplayable);
    EXPECT_EQ(toMp3.action, Action::Ask); // D-5: ask each time
    EXPECT_EQ(toMp3.notices, (QList<Notice>{Notice::NotPlayable, Notice::Generation}));
    EXPECT_EQ(describeNotice(Notice::NotPlayable, vorbis, mp3(), toMp3).shortText,
              u"rekordbox can't play Ogg Vorbis"_s);
    const Advice toAiff = filing(vorbis, output(OutputFormat::Aiff));
    EXPECT_EQ(toAiff.notices, (QList<Notice>{Notice::NotPlayable, Notice::LossyToLossless}));
    const Advice kept = filing(vorbis, output(OutputFormat::KeepAudio));
    EXPECT_EQ(kept.action, Action::KeepAsIs);
    EXPECT_EQ(kept.notices, QList<Notice>{Notice::NotPlayable});

    SourceQuality wma = lossy(Codec::Other, 192, true, Container::Other);
    wma.codecName = u"wmav2"_s;
    EXPECT_EQ(describeNotice(Notice::NotPlayable, wma, mp3(), filing(wma, mp3())).shortText,
              u"rekordbox can't play WMA"_s);
    SourceQuality opus = lossy(Codec::Opus, 160, false, Container::Ogg);
    opus.sampleRate = 48000;
    EXPECT_FALSE(filing(opus, mp3()).hasNotice(Notice::Resample)); // 48 kHz is an MP3 rate
}

TEST(Advisor, OtherLosslessAndKeep)
{
    SourceQuality wavpack = lossless(16, 44100, Container::Other, Codec::Other);
    wavpack.codecName = u"wavpack"_s;
    const Advice toMp3 = filing(wavpack, mp3());
    EXPECT_EQ(toMp3.sourceClass, SourceClass::OtherLossless);
    EXPECT_EQ(toMp3.action, Action::Convert);
    EXPECT_EQ(toMp3.notices, QList<Notice>{Notice::NotPlayable}); // explains why
    EXPECT_EQ(describeNotice(Notice::NotPlayable, wavpack, mp3(), toMp3).shortText, u"rekordbox can't play WavPack"_s);
    EXPECT_EQ(filing(wavpack, output(OutputFormat::Aiff)).action, Action::Convert);
    EXPECT_EQ(manual(wavpack, output(OutputFormat::Aiff, 24)).action, Action::Refuse);
    EXPECT_EQ(filing(lossless(16, 44100, Container::Ogg, Codec::Flac), mp3()).sourceClass, SourceClass::OtherLossless);

    const Advice keepWav = filing(lossless(16, 44100, Container::Wav, Codec::Pcm), output(OutputFormat::KeepAudio));
    EXPECT_EQ(keepWav.action, Action::KeepAsIs);
    EXPECT_EQ(keepWav.notices, QList<Notice>{Notice::WavTags});
    EXPECT_DOUBLE_EQ(keepWav.sizeFactor, 1.0);
}

TEST(Advisor, WarningsComeBeforeInformation)
{
    EncodeSettings wav16 = output(OutputFormat::Wav, 16);
    wav16.maxSampleRate = 192000;
    const Advice advice = manual(lossless(24, 192000), wav16);
    EXPECT_EQ(advice.notices, (QList<Notice>{Notice::HardwareRate, Notice::WavTags, Notice::Dither}));
    EXPECT_TRUE(advice.needsConfirmation());
}

TEST(Advisor, NoticeCopy)
{
    const SourceQuality cd = lossless(16, 44100);
    const EncodeSettings flac96 = output(OutputFormat::Flac, 0, 96000);
    const Advice upsample = manual(cd, flac96);
    const NoticeText text = describeNotice(Notice::Upsample, cd, flac96, upsample);
    EXPECT_EQ(text.shortText, u"No quality gain — larger file"_s);
    EXPECT_TRUE(text.detail.startsWith(u"Raising 44.1 kHz to 96 kHz adds no information"_s))
        << text.detail.toStdString();
    const Advice upDepth = manual(cd, output(OutputFormat::Aiff, 24));
    EXPECT_EQ(describeNotice(Notice::UpDepth, cd, output(OutputFormat::Aiff, 24), upDepth).detail,
              u"Converting 16-bit to 24-bit only pads with zeros."_s);
    const SourceQuality hiRes = lossless(24, 96000);
    const Advice resample = filing(hiRes, mp3());
    EXPECT_EQ(describeNotice(Notice::Resample, hiRes, mp3(), resample).shortText, u"Resampled to 44.1 kHz"_s);
    EXPECT_TRUE(
        describeNotice(Notice::Resample, hiRes, mp3(), resample).detail.startsWith(u"MP3 cannot store 96 kHz"_s));
    for (const Notice notice : {Notice::LossyToLossless, Notice::LossyUp, Notice::Generation, Notice::Upsample,
                                Notice::UpDepth, Notice::Suspect, Notice::NotPlayable, Notice::HardwareRate,
                                Notice::WavTags, Notice::Resample, Notice::Dither, Notice::LosslessReduction}) {
        EXPECT_FALSE(describeNotice(notice, hiRes, mp3(), resample).shortText.isEmpty());
    }
}

// R1: raising the rate gains nothing, so an MP3 above the source's rate asks first (W-UPSAMPLE), whether the rate was
// chosen or the source is below what MP3 stores (D-4 takes it to 44.1 kHz). Lowering it is only information.
TEST(Advisor, Mp3AboveTheSourceRateAsks)
{
    const SourceQuality low = lossless(16, 22050);
    for (const Advice &advice : {filing(low, mp3()), manual(low, mp3()), manual(low, vbr(0)), filing(low, mp3(128))}) {
        EXPECT_EQ(advice.sourceClass, SourceClass::LosslessGenuine);
        EXPECT_EQ(advice.action, Action::Ask);
        EXPECT_EQ(advice.notices, QList<Notice>{Notice::Upsample});
        EXPECT_EQ(advice.outputSampleRate, 44100);
        EXPECT_EQ(advice.outputBitDepth, 0);
    }
    for (const int rate : {8000, 11025, 16000, 24000, 37800}) {
        const Advice advice = filing(lossless(16, rate), mp3());
        EXPECT_EQ(advice.action, Action::Ask) << rate;
        EXPECT_EQ(advice.notices, QList<Notice>{Notice::Upsample}) << rate;
    }
    const Advice to32 = manual(low, mp3At(32000));
    EXPECT_EQ(to32.action, Action::Ask);
    EXPECT_EQ(to32.notices, QList<Notice>{Notice::Upsample});
    EXPECT_EQ(to32.outputSampleRate, 32000);
    // The rates MP3 stores are kept as they are.
    for (const int rate : {32000, 44100, 48000}) {
        const Advice advice = filing(lossless(16, rate), mp3());
        EXPECT_EQ(advice.action, Action::Convert) << rate;
        EXPECT_TRUE(advice.notices.isEmpty()) << rate;
        EXPECT_EQ(advice.outputSampleRate, rate);
    }
    // Chosen rates: above the source's asks, below it informs, the source's own is silent.
    const Advice up = manual(lossless(16, 44100), mp3At(48000));
    EXPECT_EQ(up.action, Action::Ask);
    EXPECT_EQ(up.notices, QList<Notice>{Notice::Upsample});
    EXPECT_EQ(up.outputSampleRate, 48000);
    const Advice down = manual(lossless(16, 48000), mp3At(44100));
    EXPECT_EQ(down.action, Action::Convert);
    EXPECT_EQ(down.notices, QList<Notice>{Notice::Resample});
    const Advice same = manual(lossless(16, 44100), mp3At(44100));
    EXPECT_EQ(same.action, Action::Convert);
    EXPECT_TRUE(same.notices.isEmpty());
    const Advice hiRes = manual(lossless(24, 96000), mp3At(48000));
    EXPECT_EQ(hiRes.action, Action::Convert);
    EXPECT_EQ(hiRes.notices, QList<Notice>{Notice::Resample});

    // Suspect and unplayable lossless sources keep their own warnings.
    const SourceQuality fromLossy = withFinding(low, QualityFinding::LikelyFromLossy, 7900, 32);
    const Advice suspect = filing(fromLossy, mp3());
    EXPECT_EQ(suspect.action, Action::Ask);
    EXPECT_EQ(suspect.notices, (QList<Notice>{Notice::Suspect, Notice::Upsample}));
    SourceQuality wavpack = lossless(16, 22050, Container::Other, Codec::Other);
    wavpack.codecName = u"wavpack"_s;
    const Advice other = filing(wavpack, mp3());
    EXPECT_EQ(other.sourceClass, SourceClass::OtherLossless);
    EXPECT_EQ(other.action, Action::Ask);
    EXPECT_EQ(other.notices, (QList<Notice>{Notice::NotPlayable, Notice::Upsample}));

    // Lossy sources: filing keeps an MPEG-2 MP3 at 22.05 kHz as it is (nothing re-encoded, nothing raised)...
    const SourceQuality lsf = at(lossy(Codec::Mp3, 64), 22050);
    const Advice kept = filing(lsf, mp3());
    EXPECT_EQ(kept.action, Action::KeepAsIs);
    EXPECT_EQ(kept.badge, Badge::BelowTarget);
    EXPECT_TRUE(kept.notices.isEmpty());
    // ...and by hand, the warning joins the others.
    EXPECT_EQ(manual(lsf, mp3()).notices, (QList<Notice>{Notice::LossyUp, Notice::Upsample}));
    EXPECT_EQ(manual(lsf, mp3(128)).notices, (QList<Notice>{Notice::LossyUp, Notice::Upsample}));
    EXPECT_EQ(manual(at(lossy(Codec::Mp3, 160), 24000), mp3(128)).notices,
              (QList<Notice>{Notice::Generation, Notice::Upsample}));
    const Advice vorbis = filing(at(lossy(Codec::Vorbis, 96, false, Container::Ogg), 22050), mp3());
    EXPECT_EQ(vorbis.action, Action::Ask);
    EXPECT_EQ(vorbis.notices, (QList<Notice>{Notice::NotPlayable, Notice::Generation, Notice::Upsample}));
    const SourceQuality aac = lossy(Codec::Aac, 256, false, Container::Mp4);
    EXPECT_EQ(manual(aac, mp3At(48000)).notices, (QList<Notice>{Notice::Generation, Notice::Upsample}));
    EXPECT_EQ(manual(aac, mp3At(32000)).notices, (QList<Notice>{Notice::Generation, Notice::Resample}));
    EXPECT_EQ(manual(aac, output(OutputFormat::Aiff, 0, 48000)).action, Action::Refuse); // unchanged: lossless ⛔
}

// R4: an MP3 at the target's bitrate is kept as it is only at the rate the target asks for.
TEST(Advisor, Mp3KeptOnlyAtTheTargetRate)
{
    const SourceQuality top = lossy(Codec::Mp3, 320);
    for (const EncodeSettings &target : {mp3(), mp3At(44100)}) {
        EXPECT_EQ(manual(top, target).action, Action::KeepAsIs);
        EXPECT_EQ(filing(top, target).action, Action::KeepAsIs);
        EXPECT_EQ(filing(top, target).badge, Badge::None);
    }
    const Advice raised = manual(top, mp3At(48000));
    EXPECT_EQ(raised.action, Action::Ask);
    EXPECT_EQ(raised.notices, (QList<Notice>{Notice::Generation, Notice::Upsample}));
    EXPECT_EQ(raised.outputSampleRate, 48000);
    const Advice lowered = manual(top, mp3At(32000));
    EXPECT_EQ(lowered.action, Action::Ask);
    EXPECT_EQ(lowered.notices, (QList<Notice>{Notice::Generation, Notice::Resample}));
    EXPECT_EQ(manual(at(top, 48000), mp3()).action, Action::KeepAsIs); // 48 kHz is an MP3 rate: automatic keeps it
    EXPECT_EQ(manual(at(top, 48000), mp3At(44100)).action, Action::Ask);

    // Filing never re-encodes lossy audio: kept, and marked as not the target's format. The advice describes the
    // kept file, not the target's.
    const Advice filed = filing(top, mp3At(48000));
    EXPECT_EQ(filed.action, Action::KeepAsIs);
    EXPECT_EQ(filed.badge, Badge::FormatDiffers);
    EXPECT_TRUE(filed.notices.isEmpty());
    EXPECT_EQ(filed.outputSampleRate, 44100);
    EXPECT_EQ(filed.outputBitDepth, 0);
    EXPECT_DOUBLE_EQ(filed.sizeFactor, 1.0);
    // A V0-class file meets an MP3 320 target: another rate makes it "format differs", not "below target".
    const SourceQuality v0 = lossy(Codec::Mp3, 245, false);
    EXPECT_EQ(manual(v0, mp3()).action, Action::KeepAsIs);
    EXPECT_EQ(filing(v0, mp3At(48000)).badge, Badge::FormatDiffers);
    EXPECT_EQ(manual(v0, mp3At(48000)).notices, (QList<Notice>{Notice::Generation, Notice::Upsample}));
    // Below the target, the bitrate is what matters.
    EXPECT_EQ(filing(lossy(Codec::Mp3, 192), mp3At(48000)).badge, Badge::BelowTarget);
    EXPECT_EQ(filing(lossy(Codec::Mp3, 192), vbr(2)).badge, Badge::None);
    EncodeSettings v2At48 = vbr(2);
    v2At48.sampleRate = 48000;
    EXPECT_EQ(filing(lossy(Codec::Mp3, 192), v2At48).badge, Badge::FormatDiffers);

    // Every kept file is described as itself.
    const Advice keptAsMp3 = filing(lossy(Codec::Mp3, 192), output(OutputFormat::Aiff));
    EXPECT_EQ(keptAsMp3.action, Action::KeepAsIs);
    EXPECT_EQ(keptAsMp3.outputSampleRate, 44100);
    EXPECT_EQ(keptAsMp3.outputBitDepth, 0);
    EXPECT_DOUBLE_EQ(keptAsMp3.sizeFactor, 1.0);
    const Advice keptAac = filing(at(lossy(Codec::Aac, 256, false, Container::Mp4), 48000), mp3());
    EXPECT_EQ(keptAac.outputSampleRate, 48000);
    EXPECT_DOUBLE_EQ(keptAac.sizeFactor, 1.0);
    const Advice keptFlac = filing(lossless(24, 96000), output(OutputFormat::KeepAudio));
    EXPECT_EQ(keptFlac.outputSampleRate, 96000);
    EXPECT_EQ(keptFlac.outputBitDepth, 24);
}

// R2 and R3: the copy for raised and changed rates says what is true for the target.
TEST(Advisor, RateNoticeCopy)
{
    // An MP3's size follows its bitrate, so the upsampling notice says nothing is gained, not "larger".
    const SourceQuality low = lossless(16, 22050);
    NoticeText text = describeNotice(Notice::Upsample, low, mp3(), filing(low, mp3()));
    EXPECT_EQ(text.shortText, u"No quality gain — upsampled"_s);
    EXPECT_EQ(text.detail, u"Selecta writes MP3 at 32, 44.1 or 48 kHz, so 22.05 kHz is raised to 44.1 kHz. That adds "
                           "no information: the MP3 holds nothing above 11.025 kHz."_s);
    text = describeNotice(Notice::Upsample, low, mp3At(32000), manual(low, mp3At(32000)));
    EXPECT_TRUE(
        text.detail.startsWith(u"Selecta writes MP3 at 32, 44.1 or 48 kHz, so 22.05 kHz is raised to 32 kHz."_s))
        << text.detail.toStdString();
    const SourceQuality cd = lossless(16, 44100);
    text = describeNotice(Notice::Upsample, cd, mp3At(48000), manual(cd, mp3At(48000)));
    EXPECT_EQ(text.shortText, u"No quality gain — upsampled"_s);
    EXPECT_EQ(text.detail, u"Raising 44.1 kHz to 48 kHz adds no information; MP3 can store 44.1 kHz as it is."_s);
    const SourceQuality top = lossy(Codec::Mp3, 320);
    EXPECT_FALSE(
        describeNotice(Notice::Upsample, top, mp3At(48000), manual(top, mp3At(48000))).detail.contains(u"larger"_s));
    // A lossless target does grow.
    const EncodeSettings flac96 = output(OutputFormat::Flac, 0, 96000);
    text = describeNotice(Notice::Upsample, cd, flac96, manual(cd, flac96));
    EXPECT_EQ(text.shortText, u"No quality gain — larger file"_s);
    EXPECT_TRUE(text.detail.endsWith(u"the file becomes 2.2× larger."_s)) << text.detail.toStdString();

    // Resampling says why: a rate MP3 can't store, the user's choice, or (lossless, automatic) the players' limit.
    const SourceQuality dat = lossless(16, 48000);
    text = describeNotice(Notice::Resample, dat, mp3At(44100), manual(dat, mp3At(44100)));
    EXPECT_EQ(text.shortText, u"Resampled to 44.1 kHz"_s);
    EXPECT_EQ(text.detail, u"48 kHz becomes 44.1 kHz, as you chose; Selecta uses a high-quality SoX resampler."_s);
    text = describeNotice(Notice::Resample, top, mp3At(32000), manual(top, mp3At(32000)));
    EXPECT_EQ(text.detail, u"44.1 kHz becomes 32 kHz, as you chose; Selecta uses a high-quality SoX resampler."_s);
    const SourceQuality hiRes = lossless(24, 96000);
    for (const EncodeSettings &target : {mp3(), mp3At(48000)}) {
        text = describeNotice(Notice::Resample, hiRes, target, manual(hiRes, target));
        EXPECT_EQ(text.detail, u"MP3 cannot store 96 kHz; Selecta uses a high-quality SoX resampler."_s);
    }
    const SourceQuality ultra = lossless(24, 192000);
    text =
        describeNotice(Notice::Resample, ultra, output(OutputFormat::Aiff), filing(ultra, output(OutputFormat::Aiff)));
    EXPECT_EQ(text.shortText, u"Resampled to 96 kHz"_s);
    EXPECT_EQ(text.detail,
              u"192 kHz becomes 96 kHz, your players' limit; Selecta uses a high-quality SoX resampler."_s);
    const EncodeSettings aiff441 = output(OutputFormat::Aiff, 0, 44100);
    text = describeNotice(Notice::Resample, dat, aiff441, manual(dat, aiff441));
    EXPECT_EQ(text.detail, u"48 kHz becomes 44.1 kHz, as you chose; Selecta uses a high-quality SoX resampler."_s);
    const EncodeSettings aiff882 = output(OutputFormat::Aiff, 0, 88200);
    text = describeNotice(Notice::Resample, ultra, aiff882, manual(ultra, aiff882));
    EXPECT_EQ(text.detail, u"192 kHz becomes 88.2 kHz, as you chose; Selecta uses a high-quality SoX resampler."_s);

    // Sample rates are written exactly.
    EXPECT_EQ(describeSource(low), u"FLAC 16-bit / 22.05 kHz"_s);
    EXPECT_EQ(describeSource(lossless(16, 11025)), u"FLAC 16-bit / 11.025 kHz"_s);
    EXPECT_EQ(describeSource(lossless(24, 176400)), u"FLAC 24-bit / 176.4 kHz"_s);
}

TEST(Verdict, ClaimsAndReasons)
{
    SourceQuality top = lossy(Codec::Mp3, 320);
    QualityVerdict verdict = judgeQuality(top);
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Unchecked);
    EXPECT_EQ(verdict.claimed, u"MP3 320 kbps CBR"_s);

    QualityReport genuine;
    genuine.cutoffHz = 20241;
    genuine.sampleRate = 44100;
    genuine.lameLowpassHz = 20500;
    top.checks = genuine;
    verdict = judgeQuality(top);
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Genuine);
    EXPECT_EQ(verdict.reason, u"The spectrum reaches 20.2 kHz, as expected for MP3 320 kbps CBR."_s);

    const SourceQuality fake = withFinding(lossy(Codec::Mp3, 320), QualityFinding::LikelyUpconvertedMp3, 16742, 128);
    verdict = judgeQuality(fake);
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Suspect);
    EXPECT_TRUE(verdict.reason.startsWith(u"Likely upconverted from ≈ 128 kbps"_s)) << verdict.reason.toStdString();

    SourceQuality hiRes = lossless(24, 96000);
    QualityReport full;
    full.cutoffHz = 48000;
    full.contentBandwidthHz = 48000;
    full.sampleRate = 96000;
    full.sourceBitDepth = 24;
    full.effectiveBitDepth = 24;
    hiRes.checks = full;
    verdict = judgeQuality(hiRes);
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Genuine);
    EXPECT_EQ(verdict.claimed, u"FLAC 24-bit / 96 kHz"_s);
    EXPECT_EQ(
        verdict.reason,
        u"The spectrum reaches 48 kHz: no sign of a lossy source. All 24 bits carry audio. Content up to 48 kHz."_s);

    SourceQuality shortCd = lossless(16, 44100);
    shortCd.checks = QualityReport{};
    EXPECT_EQ(judgeQuality(shortCd).kind, QualityVerdict::Kind::Unchecked);

    SourceQuality aac = lossy(Codec::Aac, 256, false, Container::Mp4);
    QualityReport cut;
    cut.cutoffHz = 16000;
    cut.sampleRate = 44100;
    aac.checks = cut;
    verdict = judgeQuality(aac);
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Suspect);
    EXPECT_EQ(verdict.claimed, u"AAC ≈ 256 kbps"_s);
    EXPECT_TRUE(verdict.reason.startsWith(u"Likely from a lower bitrate"_s));
    EXPECT_EQ(describeSource(lossy(Codec::Mp3, 245, false)), u"MP3 VBR ≈ 245 kbps"_s);
    EXPECT_EQ(describeSource(lossless(16, 44100, Container::Wav, Codec::Pcm)), u"WAV 16-bit / 44.1 kHz"_s);
}

TEST(Probe, RealFilesEndToEnd)
{
    const QTemporaryDir dir;
    Pcm music = tones({110, 1000, 7000}, 6.0, 44100, 2, 0.08);
    const Pcm noise = whiteNoise(6.0, 7, 44100, 2, 0.25);
    for (std::size_t i = 0; i < music.samples.size(); ++i) {
        music.samples[i] += noise.samples[i];
    }
    music = quantised(music, 16);
    const QString genuinePath = writeFile(dir.path(), u"genuine.mp3"_s, makeMp3(music, {.bitrateKbps = 320}));
    const Result<SourceQuality> facts = probeSourceQuality(genuinePath, /*runChecks=*/false);
    ASSERT_TRUE(facts.ok());
    EXPECT_FALSE(facts.value().checks.has_value());
    EXPECT_EQ(facts.value().codec, Codec::Mp3);
    EXPECT_TRUE(facts.value().constantBitrate);
    EXPECT_EQ(facts.value().bitrateKbps, 320);
    EXPECT_TRUE(facts.value().encoder.startsWith(u"LAME"_s));
    EXPECT_EQ(advise(facts.value(), mp3(), AdviceContext::Filing).action, Action::KeepAsIs);

    const Result<SourceQuality> genuine = probeSourceQuality(genuinePath, /*runChecks=*/true);
    ASSERT_TRUE(genuine.ok());
    ASSERT_TRUE(genuine.value().checks.has_value());
    EXPECT_EQ(judgeQuality(genuine.value()).kind, QualityVerdict::Kind::Genuine)
        << judgeQuality(genuine.value()).reason.toStdString();

    // The same music through a 128 kbps MP3 first, then "upgraded" to 320.
    DecoderOptions native;
    native.channels = 0;
    const Pcm via128 = decodeAll(makeMp3(music, {.bitrateKbps = 128}), native).pcm;
    const QString fakePath = writeFile(dir.path(), u"fake.mp3"_s, makeMp3(via128, {.bitrateKbps = 320}));
    const Result<SourceQuality> fake = probeSourceQuality(fakePath, true);
    ASSERT_TRUE(fake.ok());
    const QualityVerdict verdict = judgeQuality(fake.value());
    EXPECT_EQ(verdict.kind, QualityVerdict::Kind::Suspect);
    EXPECT_TRUE(verdict.reason.contains(u"128 kbps"_s)) << verdict.reason.toStdString();
    const Advice advice = advise(fake.value(), mp3(), AdviceContext::Filing);
    EXPECT_EQ(advice.action, Action::KeepAsIs);
    EXPECT_EQ(advice.badge, Badge::BelowTarget);

    const Result<SourceQuality> missing = probeSourceQuality(dir.filePath(u"missing.flac"_s), true);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::OpenFailed);
    selecta::audio::CancellationFlag cancel;
    cancel.cancel();
    const Result<SourceQuality> cancelled = probeSourceQuality(genuinePath, true, &cancel);
    ASSERT_FALSE(cancelled.ok());
    EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
}
