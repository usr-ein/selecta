// SPDX-License-Identifier: GPL-3.0-or-later
// C2: transcode() and TranscodeJob end to end: LAME MP3 with its Info tag, FFmpeg FLAC/ALAC/AIFF, the WAV writer, the
// audio-preserving path, resampling and dither, and every way a conversion can stop (cancellation, a full disk, an
// existing output, a damaged source) without leaving anything behind or touching the source.
#include "convert/Transcoder.h"

#include "ConvertTestUtil.h"
#include "DecodeHelpers.h"
#include "FailingDevice.h"
#include "SyntheticAudio.h"
#include "convert/OutputVerifier.h"
#include "convert/TagReader.h"
#include "convert/detail/TranscodeSeam.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;
using selecta::audio::CancellationFlag;
using selecta::audio::Codec;
using selecta::audio::Container;
using selecta::audio::DecoderOptions;
using selecta::audio::ErrorCode;

namespace {

constexpr int kTimeoutMs = 120000;

DecoderOptions nativeLayout()
{
    DecoderOptions options;
    options.channels = 0;
    return options;
}

std::string describe(const Result<TranscodeReport> &result)
{
    return result.ok() ? std::string("ok") : result.error().toString().toStdString();
}

quint32 le32(const QByteArray &bytes, qsizetype offset)
{
    quint32 value = 0;
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    return qFromLittleEndian(value);
}

quint16 le16(const QByteArray &bytes, qsizetype offset)
{
    quint16 value = 0;
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    return qFromLittleEndian(value);
}

class Transcoding : public ::testing::Test
{
protected:
    QTemporaryDir dir;

    QString write(const QString &name, const QByteArray &bytes) const { return writeFile(dir.path(), name, bytes); }
    QString path(const QString &name) const { return dir.filePath(name); }

    // Every temporary file left in the folder (there should never be one).
    QStringList partFiles() const
    {
        return QDir(dir.path()).entryList({u"*.selecta-part"_s}, QDir::Files | QDir::Hidden | QDir::System);
    }

    static TranscodeRequest request(const QString &source, const QString &output,
                                    OutputFormat format = OutputFormat::Mp3)
    {
        TranscodeRequest request;
        request.sourcePath = source;
        request.outputPath = output;
        request.settings.format = format;
        request.tags = richModel();
        request.policy.encodedBy = u"Selecta test"_s;
        return request;
    }
};

} // namespace

TEST_F(Transcoding, FlacToMp3Cbr320WithAGenuineLameTag)
{
    const Pcm pcm = quantised(tones({440.0, 3000.0}, 3.0, 44100, 2, 0.3), 16);
    const QByteArray flac = makeFlac(pcm);
    const QString source = write(u"song.flac"_s, flac);
    const QString output = path(u"song.mp3"_s);
    std::vector<double> fractions;
    const Result<TranscodeReport> result =
        transcode(request(source, output), nullptr, [&fractions](double fraction) { fractions.push_back(fraction); });
    ASSERT_TRUE(result.ok()) << describe(result);
    const TranscodeReport &report = result.value();
    EXPECT_EQ(report.outputPath, output);
    EXPECT_TRUE(report.verification.passed());
    EXPECT_EQ(report.source.container, Container::Flac);
    EXPECT_EQ(report.outputSampleRate, 44100);
    EXPECT_EQ(report.outputBitDepth, 0);
    EXPECT_FALSE(report.resampled);
    EXPECT_FALSE(report.dithered);
    EXPECT_EQ(report.outputBytes, QFileInfo(output).size());
    EXPECT_EQ(readFile(source), flac); // the source is never touched
    EXPECT_TRUE(partFiles().isEmpty());
    ASSERT_FALSE(fractions.empty());
    EXPECT_TRUE(std::ranges::is_sorted(fractions));
    EXPECT_DOUBLE_EQ(fractions.back(), 1.0);

    // Read back independently: Selecta's MPEG parser for the LAME tag and frames, the decoder for the audio.
    const QByteArray mp3 = readFile(output);
    EXPECT_TRUE(mp3.startsWith("ID3\x03")); // ID3v2.3 first, then the Info frame
    const Decoded decoded = decodeAll(mp3, nativeLayout());
    ASSERT_EQ(decoded.open, selecta::audio::Decoder::OpenStatus::Ready);
    ASSERT_TRUE(decoded.info.lameTag.has_value());
    const selecta::audio::LameTag &tag = *decoded.info.lameTag;
    EXPECT_TRUE(tag.isInfo); // "Info": CBR
    EXPECT_TRUE(tag.encoder.startsWith(u"LAME3.100"_s)) << tag.encoder.toStdString();
    EXPECT_TRUE(tag.crcValid); // what DJ apps check before skipping the frame (no 26 ms shift)
    EXPECT_EQ(tag.encoderDelay, 576);
    EXPECT_GE(tag.encoderPadding, 0);
    EXPECT_EQ(tag.lowpassHz, 20500);
    ASSERT_TRUE(decoded.info.mpeg.has_value());
    EXPECT_EQ(decoded.info.mpeg->minBitrateKbps, 320);
    EXPECT_EQ(decoded.info.mpeg->maxBitrateKbps, 320);
    EXPECT_EQ(decoded.info.mpeg->channelMode, 1);  // joint stereo
    EXPECT_EQ(decoded.pcm.frames(), pcm.frames()); // gapless: exactly the source's length
    EXPECT_LT(rmsDifference(decoded.pcm, pcm), 0.01);

    // The tags: the policy-applied model, with Selecta's own encoder fields.
    const Result<MetadataModel> tags = readTags(output);
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().value(keys::Encoding), u"LAME3.100 -b 320"_s);
    EXPECT_EQ(tags.value().value(keys::EncodedBy), u"Selecta test"_s);
    EXPECT_EQ(tags.value().value(keys::Title), richModel().value(keys::Title));
    ASSERT_TRUE(tags.value().frontCover().has_value());
    EXPECT_EQ(tags.value().frontCover()->data, richModel().frontCover()->data);
}

TEST_F(Transcoding, HiResFlacToMp3IsResampledTo44k)
{
    const Pcm pcm = quantised(tones({1000.0, 9000.0}, 2.0, 96000, 2, 0.3), 24);
    const QString source = write(u"hires.flac"_s, makeFlac(pcm, {.bits = 24}));
    const QString output = path(u"hires.mp3"_s);
    const Result<TranscodeReport> result = transcode(request(source, output));
    ASSERT_TRUE(result.ok()) << describe(result);
    EXPECT_EQ(result.value().outputSampleRate, 44100); // D-4
    EXPECT_TRUE(result.value().resampled);
    EXPECT_FALSE(result.value().dithered); // never before MP3
    const Decoded decoded = decodeAll(readFile(output), nativeLayout());
    EXPECT_EQ(decoded.info.sampleRate, 44100);
    EXPECT_NEAR(static_cast<double>(decoded.pcm.frames()), 2.0 * 44100, 2.0);
}

TEST_F(Transcoding, VbrMp3HasAXingLameTag)
{
    const QString source = write(u"v0.wav"_s, makeWav(quantised(sine(440.0, 2.0), 16)));
    const QString output = path(u"v0.mp3"_s);
    TranscodeRequest vbr = request(source, output);
    vbr.settings.mp3.mode = Mp3Mode::Vbr;
    const Result<TranscodeReport> result = transcode(vbr);
    ASSERT_TRUE(result.ok()) << describe(result);
    const Decoded decoded = decodeAll(readFile(output), nativeLayout());
    ASSERT_TRUE(decoded.info.lameTag.has_value());
    EXPECT_FALSE(decoded.info.lameTag->isInfo); // "Xing": VBR
    EXPECT_TRUE(decoded.info.lameTag->crcValid);
    EXPECT_EQ(readTags(output).value().value(keys::Encoding), u"LAME3.100 -V 0"_s);
}

TEST_F(Transcoding, MonoStaysMonoAndSurroundIsDownmixed)
{
    const QString mono = write(u"mono.wav"_s, makeWav(quantised(sine(440.0, 1.0, 44100, 1), 16)));
    ASSERT_TRUE(transcode(request(mono, path(u"mono.mp3"_s))).ok());
    const Decoded monoMp3 = decodeAll(readFile(path(u"mono.mp3"_s)), nativeLayout());
    EXPECT_EQ(monoMp3.info.channels, 1);
    EXPECT_EQ(monoMp3.info.mpeg->channelMode, 3);

    const QString surround =
        write(u"surround.wav"_s, makeWav(quantised(sine(440.0, 1.0, 48000, 6, 0.2), 16), {.extensible = true}));
    const Result<TranscodeReport> aiff = transcode(request(surround, path(u"surround.aiff"_s), OutputFormat::Aiff));
    ASSERT_TRUE(aiff.ok()) << describe(aiff);
    EXPECT_TRUE(aiff.value().dithered); // a downmix is new sample values
    const Decoded stereo = decodeAll(readFile(path(u"surround.aiff"_s)), nativeLayout());
    EXPECT_EQ(stereo.info.channels, 2);
    EXPECT_EQ(stereo.info.sampleRate, 48000);
}

TEST_F(Transcoding, LosslessOutputsAreBitIdentical)
{
    struct Case
    {
        OutputFormat format;
        const char *name;
        Container container;
        Codec codec;
    };
    const std::array<Case, 4> cases = {{{OutputFormat::Aiff, "out.aiff", Container::Aiff, Codec::Pcm},
                                        {OutputFormat::Flac, "out.flac", Container::Flac, Codec::Flac},
                                        {OutputFormat::Wav, "out.wav", Container::Wav, Codec::Pcm},
                                        {OutputFormat::Alac, "out.m4a", Container::Mp4, Codec::Alac}}};
    for (const int bits : {16, 24}) {
        const Pcm pcm = quantised(tones({220.0, 5000.0}, 1.5, 48000, 2, 0.3), bits);
        const QString source = write(u"source%1.wav"_s.arg(bits), makeWav(pcm, {.bits = bits}));
        const Result<QByteArray> sourceDigest = pcmDigest(source);
        ASSERT_TRUE(sourceDigest.ok());
        for (const Case &c : cases) {
            SCOPED_TRACE(std::string(c.name) + " " + std::to_string(bits));
            const QString output = path(u"%1-%2"_s.arg(bits).arg(QString::fromLatin1(c.name)));
            const Result<TranscodeReport> result = transcode(request(source, output, c.format));
            ASSERT_TRUE(result.ok()) << describe(result);
            EXPECT_EQ(result.value().outputBitDepth, bits);
            EXPECT_FALSE(result.value().dithered);
            EXPECT_FALSE(result.value().verification.pcmDigest.isEmpty());
            EXPECT_EQ(result.value().verification.pcmDigest, sourceDigest.value()); // every sample identical
            EXPECT_EQ(result.value().verification.info.container, c.container);
            EXPECT_EQ(result.value().verification.info.codec, c.codec);
            EXPECT_EQ(result.value().verification.info.bitsPerSample, bits);
            const Result<MetadataModel> tags = readTags(output);
            ASSERT_TRUE(tags.ok());
            EXPECT_EQ(tags.value().value(keys::Title), richModel().value(keys::Title));
            EXPECT_TRUE(tags.value().frontCover().has_value());
        }
    }
    EXPECT_TRUE(partFiles().isEmpty());
}

TEST_F(Transcoding, ContainerHeadersAreTheDjSafeOnes)
{
    const Pcm pcm = quantised(sine(440.0, 1.0), 24);
    const QString source = write(u"source.flac"_s, makeFlac(pcm, {.bits = 24}));
    ASSERT_TRUE(transcode(request(source, path(u"out.aiff"_s), OutputFormat::Aiff)).ok());
    const QByteArray aiff = readFile(path(u"out.aiff"_s));
    EXPECT_EQ(aiff.left(4), "FORM");
    EXPECT_EQ(aiff.mid(8, 4), "AIFF"); // not AIFC, which CDJs refuse
    EXPECT_TRUE(aiff.contains("ID3 "));

    ASSERT_TRUE(transcode(request(source, path(u"out.wav"_s), OutputFormat::Wav)).ok());
    const QByteArray wav = readFile(path(u"out.wav"_s));
    EXPECT_EQ(wav.left(4), "RIFF");
    EXPECT_EQ(le32(wav, 4), static_cast<quint32>(wav.size() - 8)); // RIFF size covers every chunk TagLib added
    EXPECT_EQ(wav.mid(8, 8), "WAVEfmt ");
    EXPECT_EQ(le32(wav, 16), 16U);
    EXPECT_EQ(le16(wav, 20), 1U); // WAVE_FORMAT_PCM, never WAVE_FORMAT_EXTENSIBLE
    EXPECT_EQ(le16(wav, 22), 2U);
    EXPECT_EQ(le32(wav, 24), 44100U);
    EXPECT_EQ(le16(wav, 34), 24U);
    EXPECT_EQ(wav.mid(36, 4), "data");
    EXPECT_TRUE(wav.contains("INFO")); // what rekordbox reads
    EXPECT_TRUE(wav.contains("ID3 "));

    ASSERT_TRUE(transcode(request(source, path(u"out.flac"_s), OutputFormat::Flac)).ok());
    EXPECT_TRUE(readFile(path(u"out.flac"_s)).startsWith("fLaC")); // no ID3 tag in front
}

TEST_F(Transcoding, ReducingTo16BitDithers)
{
    const Pcm pcm = quantised(sine(1000.0, 1.0, 44100, 2, 0.5), 24);
    const QString source = write(u"deep.flac"_s, makeFlac(pcm, {.bits = 24}));
    const QString output = path(u"cd.aiff"_s);
    TranscodeRequest to16 = request(source, output, OutputFormat::Aiff);
    to16.settings.bitDepth = 16;
    const Result<TranscodeReport> result = transcode(to16);
    ASSERT_TRUE(result.ok()) << describe(result);
    EXPECT_TRUE(result.value().dithered);
    EXPECT_EQ(result.value().outputBitDepth, 16);
    const Decoded decoded = decodeAll(readFile(output), nativeLayout());
    EXPECT_EQ(decoded.info.bitsPerSample, 16);
    EXPECT_EQ(decoded.pcm.frames(), pcm.frames());
    // TPDF noise: about one 16-bit step RMS, far below the signal.
    const double error = rmsDifference(decoded.pcm, pcm);
    EXPECT_GT(error, 0.2 / 32768.0);
    EXPECT_LT(error, 2.0 / 32768.0);
}

TEST_F(Transcoding, KeepAudioRewritesOnlyTheTags)
{
    const Pcm pcm = quantised(tones({440.0, 2000.0}, 2.0, 44100, 2, 0.3), 16);
    // An MP3 with an old tag, junk before the first frame and an ID3v1 tag: the audio frames must survive untouched.
    const QByteArray mp3 = makeMp3(pcm, {.id3v2Bytes = 3000, .id3v1 = true, .junkBytes = 64});
    const QString source = write(u"keep.mp3"_s, mp3);
    const QString output = path(u"kept.mp3"_s);
    TranscodeRequest keep = request(source, output, OutputFormat::KeepAudio);
    keep.tags.set(u"REPLAYGAIN_TRACK_GAIN"_s, u"-6.50 dB"_s, Provenance::Embedded);
    keep.tags.set(u"SERATO_MARKERS_V2"_s, u"someone else's cues"_s, Provenance::Embedded);
    const Result<TranscodeReport> result = transcode(keep);
    ASSERT_TRUE(result.ok()) << describe(result);
    EXPECT_EQ(readFile(source), mp3);
    const Result<QByteArray> before = pcmDigest(source);
    const Result<QByteArray> after = pcmDigest(output);
    ASSERT_TRUE(before.ok() && after.ok());
    EXPECT_EQ(before.value(), after.value());
    const Result<MetadataModel> tags = readTags(output);
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().value(u"REPLAYGAIN_TRACK_GAIN"_s), u"-6.50 dB"_s); // kept with the audio (D-7)
    EXPECT_FALSE(tags.value().contains(u"SERATO_MARKERS_V2"_s));              // D-11
    EXPECT_FALSE(tags.value().contains(keys::EncodedBy));                     // Selecta encoded nothing
    const QByteArray written = readFile(output);
    EXPECT_TRUE(written.startsWith("ID3\x03"));
    EXPECT_NE(written.right(128).left(3), "TAG"); // ID3v1 stripped

    // FLAC and AAC in M4A (D-9) are kept the same way.
    const QString flac = write(u"keep.flac"_s, makeFlac(pcm));
    const Result<TranscodeReport> keptFlac = transcode(request(flac, path(u"kept.flac"_s), OutputFormat::KeepAudio));
    ASSERT_TRUE(keptFlac.ok()) << describe(keptFlac);
    EXPECT_EQ(keptFlac.value().verification.pcmDigest, pcmDigest(flac).value());
    const QString aac = write(u"keep.m4a"_s, makeM4a(pcm, {}));
    const Result<TranscodeReport> keptAac = transcode(request(aac, path(u"kept.m4a"_s), OutputFormat::KeepAudio));
    ASSERT_TRUE(keptAac.ok()) << describe(keptAac);
    EXPECT_EQ(readTags(path(u"kept.m4a"_s)).value().value(keys::InitialKey), u"8A"_s);
    EXPECT_TRUE(partFiles().isEmpty());
}

TEST_F(Transcoding, CancellationHalfWayLeavesNothing)
{
    const QByteArray wav = makeWav(quantised(tones({440.0, 880.0}, 20.0, 44100, 2, 0.3), 16));
    const QString source = write(u"long.wav"_s, wav);
    for (const OutputFormat format : {OutputFormat::Mp3, OutputFormat::Flac, OutputFormat::KeepAudio}) {
        const QString output = path(u"cancelled-%1"_s.arg(static_cast<int>(format)));
        CancellationFlag cancel;
        double last = 0.0;
        const Result<TranscodeReport> result =
            transcode(request(source, output, format), &cancel, [&cancel, &last](double fraction) {
                last = fraction;
                if (fraction >= 0.25) {
                    cancel.cancel();
                }
            });
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
        EXPECT_GE(last, 0.25);
        EXPECT_LT(last, 1.0);
        EXPECT_FALSE(QFileInfo::exists(output));
        EXPECT_TRUE(partFiles().isEmpty());
    }
    EXPECT_EQ(readFile(source), wav);

    CancellationFlag already;
    already.cancel();
    const Result<TranscodeReport> early = transcode(request(source, path(u"never.mp3"_s)), &already);
    ASSERT_FALSE(early.ok());
    EXPECT_EQ(early.error().code, ErrorCode::Cancelled);
}

TEST_F(Transcoding, AFullDiskFailsCleanly)
{
    const Pcm pcm = quantised(tones({440.0, 3000.0}, 3.0, 44100, 2, 0.3), 16);
    const QString source = write(u"source.flac"_s, makeFlac(pcm));
    std::atomic<int> devices = 0;
    const detail::DeviceWrapper fullDisk = [&devices](QFile &file) -> std::unique_ptr<QIODevice> {
        ++devices;
        return std::make_unique<FailingDevice>(file, 32 * 1024);
    };
    for (const OutputFormat format : {OutputFormat::Mp3, OutputFormat::Flac, OutputFormat::Aiff, OutputFormat::Wav,
                                      OutputFormat::Alac, OutputFormat::KeepAudio}) {
        SCOPED_TRACE(static_cast<int>(format));
        const QString output = path(u"full-%1"_s.arg(static_cast<int>(format)));
        const Result<TranscodeReport> result =
            detail::transcode(request(source, output, format), nullptr, {}, fullDisk);
        ASSERT_FALSE(result.ok());
        EXPECT_EQ(result.error().code, ErrorCode::DiskFull) << describe(result);
        EXPECT_FALSE(QFileInfo::exists(output));
        EXPECT_TRUE(partFiles().isEmpty());
    }
    EXPECT_EQ(devices.load(), 6);
}

TEST_F(Transcoding, RefusesToReplaceOrGuess)
{
    const QString source = write(u"source.wav"_s, makeWav(quantised(sine(440.0, 0.5), 16)));
    const QString existing = write(u"existing.mp3"_s, "not mine to replace");
    Result<TranscodeReport> result = transcode(request(source, existing));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::OutputExists);
    EXPECT_EQ(readFile(existing), "not mine to replace");

    result = transcode(request(source, source, OutputFormat::KeepAudio)); // onto itself
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::OutputExists);

    result = transcode(request(path(u"missing.flac"_s), path(u"a.mp3"_s)));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::OpenFailed);

    result = transcode(request(source, path(u"no/such/folder/a.mp3"_s)));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::OpenFailed);

    TranscodeRequest badBitrate = request(source, path(u"b.mp3"_s));
    badBitrate.settings.mp3.bitrateKbps = 300;
    result = transcode(badBitrate);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    TranscodeRequest badCover = request(source, path(u"c.mp3"_s));
    Picture garbage;
    garbage.data = "not an image";
    badCover.tags.setFrontCover(garbage);
    result = transcode(badCover);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);

    const QString noise = write(u"noise.mp3"_s, randomBytes(64 * 1024, 7));
    result = transcode(request(noise, path(u"d.mp3"_s)));
    ASSERT_FALSE(result.ok());
    EXPECT_TRUE(partFiles().isEmpty());
    for (const QString &name : {u"a.mp3"_s, u"b.mp3"_s, u"c.mp3"_s, u"d.mp3"_s}) {
        EXPECT_FALSE(QFileInfo::exists(path(name))) << name.toStdString();
    }
}

TEST_F(Transcoding, ADamagedSourceIsNotConvertedButCanBeKept)
{
    const QByteArray flac = makeFlac(quantised(tones({440.0, 3000.0}, 3.0, 44100, 2, 0.3), 16));
    const QString source = write(u"truncated.flac"_s, flac.left(flac.size() * 2 / 3)); // an interrupted download
    const Result<TranscodeReport> converted = transcode(request(source, path(u"truncated.mp3"_s)));
    ASSERT_FALSE(converted.ok());
    EXPECT_EQ(converted.error().code, ErrorCode::Corrupt);
    EXPECT_FALSE(QFileInfo::exists(path(u"truncated.mp3"_s)));
    // Kept as it is, the copy has exactly the same audio, damage included.
    const Result<TranscodeReport> kept = transcode(request(source, path(u"kept.flac"_s), OutputFormat::KeepAudio));
    ASSERT_TRUE(kept.ok()) << describe(kept);
    EXPECT_TRUE(kept.value().verification.stats.truncated);
    EXPECT_TRUE(partFiles().isEmpty());
}

TEST_F(Transcoding, ParallelConversionsAreIndependent)
{
    // The conversion workers run several jobs at once (CONV-8): LAME's initialisation is serialised, everything else
    // belongs to one job. Each output must be byte for byte what the same conversion gives alone.
    constexpr int kJobs = 4;
    std::vector<QString> sources;
    std::vector<QByteArray> alone;
    for (int i = 0; i < kJobs; ++i) {
        const Pcm pcm = quantised(tones({220.0 * (i + 1), 3000.0}, 1.5, 44100, 2, 0.3), 16);
        sources.push_back(write(u"parallel%1.flac"_s.arg(i), makeFlac(pcm)));
        const OutputFormat format = i % 2 == 0 ? OutputFormat::Mp3 : OutputFormat::Flac;
        const QString output = path(u"alone%1"_s.arg(i));
        ASSERT_TRUE(transcode(request(sources.back(), output, format)).ok());
        alone.push_back(readFile(output));
    }
    std::vector<std::thread> threads;
    threads.reserve(kJobs);
    std::vector<int> succeeded(kJobs, 0);
    for (int i = 0; i < kJobs; ++i) {
        threads.emplace_back([&, i] {
            const OutputFormat format = i % 2 == 0 ? OutputFormat::Mp3 : OutputFormat::Flac;
            succeeded[static_cast<std::size_t>(i)] =
                transcode(request(sources[static_cast<std::size_t>(i)], path(u"together%1"_s.arg(i)), format)).ok() ? 1
                                                                                                                    : 0;
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    for (int i = 0; i < kJobs; ++i) {
        EXPECT_EQ(succeeded[static_cast<std::size_t>(i)], 1) << i;
        EXPECT_EQ(readFile(path(u"together%1"_s.arg(i))), alone[static_cast<std::size_t>(i)]) << i;
    }
    EXPECT_TRUE(partFiles().isEmpty());
}

// ---------------------------------------------------------------------------------------------------------------
// TranscodeJob

TEST_F(Transcoding, JobReportsProgressAndFinishes)
{
    const QString source = write(u"job.wav"_s, makeWav(quantised(tones({440.0, 880.0}, 10.0, 44100, 2, 0.3), 16)));
    const QString output = path(u"job.mp3"_s);
    TranscodeJob job(request(source, output));
    const QSignalSpy finished(&job, &TranscodeJob::finished);
    const QSignalSpy failed(&job, &TranscodeJob::failed);
    std::vector<double> fractions;
    QObject::connect(&job, &TranscodeJob::progress, &job,
                     [&fractions](double fraction) { fractions.push_back(fraction); });
    job.start();
    job.start(); // once only
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() + failed.count() > 0; }, kTimeoutMs));
    ASSERT_EQ(finished.count(), 1)
        << (failed.isEmpty() ? std::string()
                             : failed.first().first().value<selecta::audio::Error>().toString().toStdString());
    const auto report = finished.first().first().value<TranscodeReport>();
    EXPECT_EQ(report.outputPath, output);
    EXPECT_TRUE(QFileInfo::exists(output));
    EXPECT_TRUE(std::ranges::is_sorted(fractions));
    for (const double fraction : fractions) {
        EXPECT_GE(fraction, 0.0);
        EXPECT_LE(fraction, 1.0);
    }
    QTest::qWait(50);
    EXPECT_EQ(finished.count(), 1);
    EXPECT_EQ(failed.count(), 0);
}

TEST_F(Transcoding, JobFailureIsReported)
{
    TranscodeJob job(request(path(u"missing.wav"_s), path(u"missing.mp3"_s)));
    const QSignalSpy finished(&job, &TranscodeJob::finished);
    const QSignalSpy failed(&job, &TranscodeJob::failed);
    job.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return finished.count() + failed.count() > 0; }, kTimeoutMs));
    ASSERT_EQ(failed.count(), 1);
    EXPECT_EQ(failed.first().first().value<selecta::audio::Error>().code, ErrorCode::OpenFailed);
}

TEST_F(Transcoding, CancelledJobEmitsNothingMoreAndCleansUp)
{
    const QString source = write(u"long.wav"_s, makeWav(quantised(tones({440.0, 880.0}, 60.0, 44100, 2, 0.3), 16)));
    const QString output = path(u"cancelled.mp3"_s);
    TranscodeJob job(request(source, output));
    const QSignalSpy finished(&job, &TranscodeJob::finished);
    const QSignalSpy failed(&job, &TranscodeJob::failed);
    int progressAfterCancel = 0;
    bool cancelled = false;
    QObject::connect(&job, &TranscodeJob::progress, &job, [&](double) {
        if (cancelled) {
            ++progressAfterCancel;
            return;
        }
        cancelled = true;
        job.cancel(); // from the first progress report: the worker is mid-encode
    });
    job.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return cancelled; }, kTimeoutMs));
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(kTimeoutMs));
    QTest::qWait(50); // deliver whatever the worker posted
    EXPECT_EQ(finished.count(), 0);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(progressAfterCancel, 0);
    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_TRUE(partFiles().isEmpty());
}

TEST_F(Transcoding, DestroyingARunningJobCancelsIt)
{
    const QString source = write(u"long.wav"_s, makeWav(quantised(tones({440.0, 880.0}, 60.0, 44100, 2, 0.3), 16)));
    const QString output = path(u"destroyed.mp3"_s);
    auto job = std::make_unique<TranscodeJob>(request(source, output));
    bool started = false;
    QObject::connect(job.get(), &TranscodeJob::progress, job.get(), [&started](double) { started = true; });
    job->start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return started; }, kTimeoutMs));
    job.reset();
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(kTimeoutMs));
    QTest::qWait(50);
    EXPECT_FALSE(QFileInfo::exists(output));
    EXPECT_TRUE(partFiles().isEmpty());
}
