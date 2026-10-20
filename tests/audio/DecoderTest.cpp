// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder (A1): every format on complete and growing sources, exact seeking, gapless MP3, resampling and channel
// mapping, and the failure paths (truncation, garbage, empty files, failed downloads, hostile headers).
#include "audio/Decoder.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"

#include <QtEndian>

#include <cmath>
#include <gtest/gtest.h>

using namespace selecta::audio;
using namespace selecta::testing;

namespace {

constexpr int kMiB = 1024 * 1024;

float maxAbsDifference(const Pcm &a, const Pcm &b, qint64 frames, qint64 offsetA = 0, qint64 offsetB = 0)
{
    float worst = 0;
    const int channels = std::min(a.channels, b.channels);
    for (qint64 i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            worst = std::max(worst, std::abs(a.at(offsetA + i, c) - b.at(offsetB + i, c)));
        }
    }
    return worst;
}

DecoderOptions native()
{
    DecoderOptions options;
    options.channels = 0;
    return options;
}

void expectExact(const Pcm &decoded, const Pcm &expected)
{
    ASSERT_EQ(decoded.frames(), expected.frames());
    ASSERT_EQ(decoded.channels, expected.channels);
    EXPECT_EQ(maxAbsDifference(decoded, expected, expected.frames()), 0.0F);
}

} // namespace

// --- Lossless formats decode bit-exactly ------------------------------------------------------------------------

TEST(Decoder, WavIntegerAndFloatAreExact)
{
    const Pcm source = logSweep(40, 16000, 1.5, 44100, 2);
    for (const int bits : {8, 16, 24, 32}) {
        const Pcm expected = quantised(source, bits);
        const Decoded d = decodeAll(makeWav(expected, {.bits = bits}), native());
        ASSERT_EQ(d.last, DecodeStatus::EndOfStream) << bits << " " << d.error.toString().toStdString();
        EXPECT_EQ(d.info.container, Container::Wav);
        EXPECT_EQ(d.info.codec, Codec::Pcm);
        EXPECT_EQ(d.info.bitsPerSample, bits);
        EXPECT_TRUE(d.info.lossless);
        EXPECT_TRUE(d.info.exactLength);
        EXPECT_EQ(d.lengthFrames, expected.frames());
        if (bits == 32) {
            // 32-bit integers don't fit a float exactly; the error stays below one 24-bit step.
            EXPECT_LT(maxAbsDifference(d.pcm, expected, expected.frames()), 1.0F / 8388608.0F);
        } else {
            expectExact(d.pcm, expected);
        }
    }
    const Decoded f = decodeAll(makeWav(source, {.bits = 32, .floatingPoint = true}), native());
    EXPECT_TRUE(f.info.floatingPoint);
    expectExact(f.pcm, source);
    const Decoded e = decodeAll(makeWav(quantised(source, 24), {.bits = 24, .extensible = true}), native());
    expectExact(e.pcm, quantised(source, 24));
}

TEST(Decoder, AiffVariantsAreExact)
{
    const Pcm source = quantised(tones({100, 1000, 9000}, 1.0, 48000, 2), 24);
    expectExact(decodeAll(makeAiff(source, {.bits = 24}), native()).pcm, source);
    const Pcm sixteen = quantised(source, 16);
    expectExact(decodeAll(makeAiff(sixteen, {.bits = 16, .compression = "sowt"}), native()).pcm, sixteen);
    expectExact(decodeAll(makeAiff(source, {.compression = "fl32"}), native()).pcm, source);
    const Decoded late = decodeAll(makeAiff(sixteen, {.bits = 16, .commAfterSsnd = true}), native());
    expectExact(late.pcm, sixteen);
    EXPECT_EQ(late.info.container, Container::Aiff);
}

TEST(Decoder, FlacIsExactAt16And24Bits)
{
    for (const int bits : {16, 24}) {
        const Pcm source = quantised(whiteNoise(2.0, 42, 96000, 2, 0.5), bits);
        const Decoded d = decodeAll(makeFlac(source, {.bits = bits}), native());
        ASSERT_EQ(d.last, DecodeStatus::EndOfStream) << d.error.toString().toStdString();
        EXPECT_EQ(d.info.codec, Codec::Flac);
        EXPECT_EQ(d.info.bitsPerSample, bits);
        EXPECT_EQ(d.info.sampleRate, 96000);
        EXPECT_TRUE(d.info.exactLength);
        expectExact(d.pcm, source);
        EXPECT_EQ(d.stats.decodeErrors, 0);
    }
}

TEST(Decoder, FlacWithLargePictureAndId3Prefix)
{
    const Pcm source = quantised(sine(440, 1.0), 16);
    const QByteArray flac = makeFlac(source, {.pictureBytes = 3 * kMiB, .id3v2Bytes = 50000});
    expectExact(decodeAll(flac, native()).pcm, source);
    std::shared_ptr<LoggingSource> logging;
    const Decoded growing = decodeProgressively(flac, 65536, native(), &logging);
    expectExact(growing.pcm, source);
    EXPECT_EQ(logging->violations(), 0);
}

TEST(Decoder, AlacAndOggFlacThroughFfmpeg)
{
    const Pcm source = quantised(tones({220, 3000}, 1.0, 44100, 2), 16);
    const Decoded alac = decodeAll(makeM4a(source, {.alac = true}), native());
    ASSERT_EQ(alac.last, DecodeStatus::EndOfStream) << alac.error.toString().toStdString();
    EXPECT_EQ(alac.info.codec, Codec::Alac);
    EXPECT_EQ(alac.info.container, Container::Mp4);
    EXPECT_TRUE(alac.info.lossless);
    EXPECT_EQ(alac.info.bitsPerSample, 16); // from the decoder: the depth is in ALAC's magic cookie
    expectExact(alac.pcm, source);
    const Decoded ogg = decodeAll(makeOggFlac(source), native());
    ASSERT_EQ(ogg.last, DecodeStatus::EndOfStream) << ogg.error.toString().toStdString();
    EXPECT_EQ(ogg.info.container, Container::Ogg);
    expectExact(ogg.pcm, source);
}

// --- Lossy formats ----------------------------------------------------------------------------------------------

TEST(Decoder, LameMp3IsGapless)
{
    const Pcm source = sine(1000, 3.0, 44100, 2);
    for (const Mp3Options &options : {Mp3Options{}, Mp3Options{.vbr = true, .vbrQuality = 0}}) {
        const Decoded d = decodeAll(makeMp3(source, options), native());
        ASSERT_EQ(d.last, DecodeStatus::EndOfStream) << d.error.toString().toStdString();
        EXPECT_EQ(d.info.codec, Codec::Mp3);
        ASSERT_TRUE(d.info.lameTag);
        EXPECT_TRUE(d.info.lameTag->crcValid);
        EXPECT_TRUE(d.info.exactLength);
        // Encoder delay and padding are removed: exactly the source length, and aligned with it.
        EXPECT_EQ(d.pcm.frames(), source.frames());
        EXPECT_EQ(d.lengthFrames, source.frames());
        EXPECT_LT(rmsDifference(d.pcm, source, 2048), 0.01);
        ASSERT_TRUE(d.info.mpeg);
        if (!options.vbr) {
            EXPECT_TRUE(d.info.mpeg->constantBitrate());
            EXPECT_EQ(d.info.mpeg->maxBitrateKbps, 320);
            EXPECT_EQ(d.info.bitrateKbps, 320);
        }
    }
}

TEST(Decoder, Mp3WithoutLameTagKeepsEncoderDelay)
{
    const Pcm source = sine(1000, 2.0, 44100, 2);
    const Decoded d = decodeAll(makeMp3(source, {.lameTag = false}), native());
    ASSERT_EQ(d.last, DecodeStatus::EndOfStream);
    EXPECT_FALSE(d.info.lameTag);
    EXPECT_GE(d.pcm.frames(), source.frames());
    EXPECT_EQ(d.pcm.frames() % 1152, 0);
}

TEST(Decoder, Mp3WithLargeId3TagJunkAndId3v1)
{
    const Pcm source = sine(440, 2.0, 44100, 2);
    const QByteArray plain = makeMp3(source, {});
    const QByteArray tagged = makeMp3(source, {.id3v2Bytes = 2 * kMiB, .id3v1 = true, .junkBytes = 300});
    const Decoded a = decodeAll(plain, native());
    const Decoded b = decodeAll(tagged, native());
    ASSERT_EQ(b.last, DecodeStatus::EndOfStream) << b.error.toString().toStdString();
    expectExact(b.pcm, a.pcm);
    EXPECT_EQ(b.info.audioOffset, (2 * kMiB) + 300 + (plain.size() - a.info.audioBytes));
    std::shared_ptr<LoggingSource> logging;
    const Decoded c = decodeProgressively(tagged, 100000, native(), &logging);
    expectExact(c.pcm, a.pcm);
    EXPECT_EQ(logging->violations(), 0);
}

TEST(Decoder, AacInMp4)
{
    const Pcm source = sine(1000, 3.0, 44100, 2);
    const Decoded d = decodeAll(makeM4a(source, {}), native());
    ASSERT_EQ(d.last, DecodeStatus::EndOfStream) << d.error.toString().toStdString();
    EXPECT_EQ(d.info.codec, Codec::Aac);
    EXPECT_NEAR(static_cast<double>(d.pcm.frames()), static_cast<double>(source.frames()), 2048.0);
    EXPECT_GT(rms(d.pcm), 0.3);
}

// --- Growing files ---------------------------------------------------------------------------------------------

TEST(Decoder, GrowingFilesDecodeLikeCompleteOnes)
{
    const Pcm source = quantised(tones({60, 1000, 8000}, 4.0, 44100, 2), 16);
    const std::vector<std::pair<const char *, QByteArray>> files = {
        {"wav", makeWav(source, {.listChunkBytes = 1000})},
        {"aiff", makeAiff(source, {.id3ChunkBytes = 2000})},
        {"flac", makeFlac(source)},
        {"mp3 cbr", makeMp3(source, {})},
        {"mp3 vbr", makeMp3(source, {.vbr = true, .vbrQuality = 2})},
        {"m4a fast start", makeM4a(source, {.fastStart = true})},
    };
    for (const auto &[name, file] : files) {
        const Decoded complete = decodeAll(file, native());
        ASSERT_EQ(complete.last, DecodeStatus::EndOfStream) << name;
        for (const qint64 step : {qint64{4093}, qint64{65536}}) {
            std::shared_ptr<LoggingSource> logging;
            const Decoded growing = decodeProgressively(file, step, native(), &logging);
            ASSERT_EQ(growing.last, DecodeStatus::EndOfStream) << name << " " << growing.error.toString().toStdString();
            EXPECT_EQ(logging->violations(), 0) << name;
            ASSERT_EQ(growing.pcm.frames(), complete.pcm.frames()) << name;
            EXPECT_LT(maxAbsDifference(growing.pcm, complete.pcm, complete.pcm.frames()), 1e-6F) << name;
        }
    }
}

TEST(Decoder, Mp4WithIndexAtTheEndNeedsTheCompleteFile)
{
    const QByteArray m4a = makeM4a(sine(440, 2.0), {});
    const auto source = std::make_shared<MemorySource>(m4a, m4a.size() - 1);
    Decoder decoder;
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::NeedsCompleteFile);
    EXPECT_EQ(decoder.error().code, ErrorCode::NeedsCompleteFile);
    source->setComplete();
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::Ready);
}

TEST(Decoder, AiffWithCommAfterSsndNeedsTheCompleteFile)
{
    const QByteArray aiff = makeAiff(sine(440, 1.0), {.commAfterSsnd = true});
    const auto source = std::make_shared<MemorySource>(aiff, aiff.size() / 2);
    Decoder decoder;
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::NeedsCompleteFile);
    source->setComplete();
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::Ready);
}

TEST(Decoder, OpenWaitsForTheHeader)
{
    const QByteArray wav = makeWav(sine(440, 1.0), {.listChunkBytes = 50000});
    const auto source = std::make_shared<MemorySource>(wav, 40000);
    Decoder decoder;
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::NeedMoreData);
    source->setAvailable(60000);
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::Ready);
    EXPECT_GT(decoder.decodableFrames(), 0);
    EXPECT_LT(decoder.decodableFrames(), decoder.lengthFrames());
    source->setComplete();
    EXPECT_EQ(decoder.decodableFrames(), decoder.lengthFrames());
}

TEST(Decoder, FailedDownloadEndsWithSourceFailed)
{
    const QByteArray mp3 = makeMp3(sine(440, 5.0), {});
    const auto source = std::make_shared<MemorySource>(mp3, mp3.size() / 2);
    Decoder decoder;
    ASSERT_EQ(decoder.open(source), Decoder::OpenStatus::Ready);
    std::vector<float> buffer(8192);
    qint64 frames = 0;
    ReadResult r;
    do {
        r = decoder.read(buffer.data(), 4096);
        frames += r.frames;
    } while (r.status == DecodeStatus::Ok);
    EXPECT_EQ(r.status, DecodeStatus::NeedMoreData);
    EXPECT_GT(frames, 0);
    source->setFailed();
    r = decoder.read(buffer.data(), 4096);
    EXPECT_EQ(r.status, DecodeStatus::Error);
    EXPECT_EQ(decoder.error().code, ErrorCode::SourceFailed);
}

TEST(Decoder, FailedDownloadBeforeTheHeader)
{
    const QByteArray wav = makeWav(sine(440, 1.0));
    const auto source = std::make_shared<MemorySource>(wav, 10);
    source->setFailed();
    Decoder decoder;
    EXPECT_EQ(decoder.open(source), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::SourceFailed);
}

// --- Seeking -------------------------------------------------------------------------------------------------

TEST(Decoder, SeeksAreExact)
{
    const Pcm source = quantised(logSweep(30, 18000, 6.0, 44100, 2), 16);
    struct Case
    {
        const char *name = nullptr;
        QByteArray file;
        float tolerance = 0.0F;
    };
    const std::vector<Case> cases = {
        {"wav", makeWav(source), 0.0F},          {"aiff", makeAiff(source), 0.0F},
        {"flac", makeFlac(source), 0.0F},        {"alac", makeM4a(source, {.alac = true}), 0.0F},
        {"mp3 cbr", makeMp3(source, {}), 1e-6F}, {"mp3 vbr", makeMp3(source, {.vbr = true}), 1e-6F},
    };
    for (const Case &c : cases) {
        const Decoded full = decodeAll(c.file, native());
        ASSERT_EQ(full.last, DecodeStatus::EndOfStream) << c.name;
        Decoder decoder;
        ASSERT_EQ(decoder.open(std::make_shared<MemorySource>(c.file), native()), Decoder::OpenStatus::Ready) << c.name;
        EXPECT_TRUE(decoder.info().exactSeek) << c.name;
        for (const qint64 target : {qint64{0}, qint64{1}, qint64{1151}, qint64{50000}, qint64{123457}, qint64{200000},
                                    full.pcm.frames() - 3000}) {
            ASSERT_EQ(decoder.seek(target), DecodeStatus::Ok) << c.name << " " << target;
            EXPECT_EQ(decoder.position(), target);
            const Pcm part = readFrames(decoder, 2000);
            ASSERT_EQ(part.frames(), 2000) << c.name << " " << target;
            EXPECT_LE(maxAbsDifference(part, full.pcm, 2000, 0, target), c.tolerance) << c.name << " at " << target;
        }
        // Past the end: nothing more to read.
        ASSERT_EQ(decoder.seek(full.pcm.frames() + 100000), DecodeStatus::Ok);
        std::vector<float> buffer(4096);
        EXPECT_EQ(decoder.read(buffer.data(), 1024).status, DecodeStatus::EndOfStream) << c.name;
    }
}

TEST(Decoder, AacSeeksLandOnTheTarget)
{
    const Pcm source = sine(500, 4.0, 44100, 1);
    const QByteArray m4a = makeM4a(source, {});
    const Decoded full = decodeAll(m4a, native());
    Decoder decoder;
    ASSERT_EQ(decoder.open(std::make_shared<MemorySource>(m4a), native()), Decoder::OpenStatus::Ready);
    for (const qint64 target : {qint64{10000}, qint64{88200}}) {
        ASSERT_EQ(decoder.seek(target), DecodeStatus::Ok);
        const Pcm part = readFrames(decoder, 4096);
        EXPECT_LT(maxAbsDifference(part, full.pcm, 4096, 0, target), 1e-3F) << target;
    }
}

TEST(Decoder, SeekBeyondTheDownloadedPartWaits)
{
    const Pcm source = quantised(logSweep(30, 18000, 6.0, 44100, 2), 16);
    for (const QByteArray &file : {makeFlac(source), makeMp3(source, {}), makeWav(source)}) {
        const Decoded full = decodeAll(file, native());
        const auto growing = std::make_shared<LoggingSource>(file, file.size() / 4);
        Decoder decoder;
        ASSERT_EQ(decoder.open(growing, native()), Decoder::OpenStatus::Ready);
        const qint64 target = (full.pcm.frames() * 3) / 4;
        EXPECT_EQ(decoder.seek(target), DecodeStatus::NeedMoreData);
        EXPECT_EQ(decoder.position(), target);
        std::vector<float> buffer(8192);
        EXPECT_EQ(decoder.read(buffer.data(), 1024).status, DecodeStatus::NeedMoreData);
        growing->setAvailable(file.size() - 1000);
        const Pcm part = readFrames(decoder, 2000);
        ASSERT_EQ(part.frames(), 2000);
        EXPECT_LE(maxAbsDifference(part, full.pcm, 2000, 0, target), 1e-6F);
        EXPECT_EQ(growing->violations(), 0);
    }
}

// --- Output format ----------------------------------------------------------------------------------------------

TEST(Decoder, MonoMixAndStereoDuplication)
{
    const Pcm stereo = quantised(stereoPair(440, 660, 1.0), 16);
    DecoderOptions mono;
    mono.channels = 1;
    const Decoded m = decodeAll(makeWav(stereo), mono);
    ASSERT_EQ(m.pcm.channels, 1);
    ASSERT_EQ(m.pcm.frames(), stereo.frames());
    for (qint64 i = 0; i < stereo.frames(); i += 997) {
        EXPECT_EQ(m.pcm.at(i, 0), 0.5F * (stereo.at(i, 0) + stereo.at(i, 1)));
    }
    const Pcm single = quantised(sine(440, 0.5, 44100, 1), 16);
    const Decoded s = decodeAll(makeWav(single), DecoderOptions{});
    ASSERT_EQ(s.pcm.channels, 2);
    for (qint64 i = 0; i < single.frames(); i += 101) {
        EXPECT_EQ(s.pcm.at(i, 0), single.at(i, 0));
        EXPECT_EQ(s.pcm.at(i, 1), single.at(i, 0));
    }
}

TEST(Decoder, ResamplesWithoutShiftingTime)
{
    Pcm impulse = silence(1.0, 44100, 1);
    impulse.samples[10000] = 0.9F;
    DecoderOptions options;
    options.channels = 1;
    options.sampleRate = 48000;
    const Decoded d = decodeAll(makeWav(impulse, {.bits = 32, .floatingPoint = true}), options);
    ASSERT_EQ(d.last, DecodeStatus::EndOfStream) << d.error.toString().toStdString();
    EXPECT_EQ(d.pcm.sampleRate, 48000);
    EXPECT_NEAR(static_cast<double>(d.pcm.frames()), 48000.0, 2.0);
    EXPECT_EQ(d.lengthFrames, 48000);
    qint64 peak = 0;
    for (qint64 i = 0; i < d.pcm.frames(); ++i) {
        if (std::abs(d.pcm.at(i, 0)) > std::abs(d.pcm.at(peak, 0))) {
            peak = i;
        }
    }
    EXPECT_NEAR(static_cast<double>(peak), 10000.0 * 48000.0 / 44100.0, 1.0);

    // A sine keeps its frequency: count zero crossings.
    const Decoded tone = decodeAll(makeWav(sine(1000, 1.0, 44100, 1), {.bits = 32, .floatingPoint = true}), options);
    int crossings = 0;
    for (qint64 i = 1; i < tone.pcm.frames(); ++i) {
        if ((tone.pcm.at(i - 1, 0) < 0) != (tone.pcm.at(i, 0) < 0)) {
            ++crossings;
        }
    }
    EXPECT_NEAR(crossings, 2000, 3);
}

TEST(Decoder, ResampledSeekPositions)
{
    const Pcm source = sine(440, 3.0, 44100, 2);
    DecoderOptions options;
    options.sampleRate = 48000;
    const QByteArray wav = makeWav(source, {.bits = 32, .floatingPoint = true});
    const Decoded full = decodeAll(wav, options);
    Decoder decoder;
    ASSERT_EQ(decoder.open(std::make_shared<MemorySource>(wav), options), Decoder::OpenStatus::Ready);
    ASSERT_EQ(decoder.seek(96000), DecodeStatus::Ok);
    const Pcm part = readFrames(decoder, 1000);
    // Within one source sample of the full decode (the resampler restarts at the seek point).
    EXPECT_LT(maxAbsDifference(part, full.pcm, 1000, 0, 96000), 0.02F);
}

// --- Failure paths ----------------------------------------------------------------------------------------------

TEST(Decoder, TruncatedFilesDecodeWhatIsThere)
{
    const Pcm source = sine(440, 3.0);
    const QByteArray mp3 = makeMp3(source, {});
    const QByteArray cutMp3 = mp3.left((mp3.size() / 2) + 100); // inside a frame
    const Decoded m = decodeAll(cutMp3, native());
    EXPECT_EQ(m.last, DecodeStatus::EndOfStream);
    EXPECT_TRUE(m.stats.truncated);
    EXPECT_GT(m.pcm.frames(), source.frames() / 3);
    EXPECT_LT(m.pcm.frames(), source.frames());

    const QByteArray wav = makeWav(source);
    const Decoded w = decodeAll(wav.left((wav.size() / 2) + 3), native());
    EXPECT_EQ(w.last, DecodeStatus::EndOfStream);
    EXPECT_TRUE(w.stats.truncated);
    EXPECT_NEAR(static_cast<double>(w.pcm.frames()), static_cast<double>(source.frames()) / 2.0, 100.0);

    const QByteArray flac = makeFlac(quantised(source, 16));
    const Decoded f = decodeAll(flac.left(flac.size() / 2), native());
    EXPECT_EQ(f.last, DecodeStatus::EndOfStream);
    EXPECT_GT(f.pcm.frames(), 0);
    EXPECT_TRUE(f.stats.truncated); // fewer samples than STREAMINFO announced

    // Cut exactly between MP3 frames: no partial frame, but fewer frames than the LAME tag counts.
    const Decoded whole = decodeAll(mp3, native());
    EXPECT_FALSE(whole.stats.truncated);
    // Walk the frames from the Info frame at offset 0: 320 kbps at 44.1 kHz is 1044 bytes plus the padding bit.
    qsizetype cut = 0;
    while (cut < mp3.size() / 2) {
        const bool padded = (static_cast<quint8>(mp3[cut + 2]) & 0x02U) != 0;
        cut += 1044 + (padded ? 1 : 0);
    }
    const Decoded between = decodeAll(mp3.left(cut), native());
    EXPECT_EQ(between.last, DecodeStatus::EndOfStream);
    EXPECT_TRUE(between.stats.truncated);
}

TEST(Decoder, GarbageEmptyAndTinyInputs)
{
    const QByteArray garbage = randomBytes(500000, 1234);
    Decoder decoder;
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(garbage)), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::Unsupported);
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(garbage, 400000)), Decoder::OpenStatus::NeedsCompleteFile);
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(QByteArray())), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::Corrupt);
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(QByteArray(100, '\0'), 0)),
              Decoder::OpenStatus::NeedMoreData);
    std::vector<float> buffer(16);
    EXPECT_EQ(decoder.read(buffer.data(), 8).status, DecodeStatus::Error); // not open
    EXPECT_EQ(decoder.open(nullptr), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::InvalidArgument);
}

TEST(Decoder, CorruptionInTheMiddleIsSurvived)
{
    const Pcm source = quantised(sine(440, 4.0), 16);
    QByteArray mp3 = makeMp3(source, {});
    QByteArray flac = makeFlac(source);
    for (int i = 0; i < 4000; ++i) {
        mp3[(mp3.size() / 2) + i] = static_cast<char>(0xFF);
        flac[(flac.size() / 2) + i] = static_cast<char>(i * 31);
    }
    const Decoded m = decodeAll(mp3, native());
    EXPECT_EQ(m.last, DecodeStatus::EndOfStream);
    EXPECT_GT(m.stats.skippedBytes + m.stats.decodeErrors, 0);
    EXPECT_GT(m.pcm.frames(), source.frames() * 9 / 10);
    const Decoded f = decodeAll(flac, native());
    EXPECT_EQ(f.last, DecodeStatus::EndOfStream);
    EXPECT_GT(f.pcm.frames(), source.frames() * 9 / 10);
}

TEST(Decoder, HostileHeadersAreBounded)
{
    // A FLAC STREAMINFO announcing 2^35 samples: refused as absurdly long.
    QByteArray flac = makeFlac(quantised(sine(440, 0.5), 16));
    flac[8 + 13] = static_cast<char>(static_cast<quint8>(flac[8 + 13]) | 0x08U); // top bit of the 36-bit sample count
    Decoder decoder;
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(flac)), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::TooLarge);

    // An implausible Xing frame count is ignored, not trusted.
    QByteArray mp3 = makeMp3(sine(440, 1.0), {});
    qToBigEndian<quint32>(0x7FFFFFFFU, mp3.data() + 44);
    const Decoded d = decodeAll(mp3, native());
    EXPECT_EQ(d.last, DecodeStatus::EndOfStream);

    // Header bounds.
    DecoderOptions tight;
    tight.maxHeaderBytes = 100000;
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(makeMp3(sine(440, 1.0), {.id3v2Bytes = 200000})), tight),
              Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::TooLarge);
}

TEST(Decoder, OptionsAreValidated)
{
    const QByteArray wav = makeWav(sine(440, 0.1));
    Decoder decoder;
    DecoderOptions options;
    options.channels = 3;
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(wav), options), Decoder::OpenStatus::Failed);
    EXPECT_EQ(decoder.error().code, ErrorCode::InvalidArgument);
    options.channels = 2;
    options.sampleRate = 100;
    EXPECT_EQ(decoder.open(std::make_shared<MemorySource>(wav), options), Decoder::OpenStatus::Failed);
}

TEST(Decoder, CloneSharesTheIndexOfCompleteSources)
{
    const Pcm source = quantised(sine(440, 3.0), 16);
    const QByteArray flac = makeFlac(source);
    Decoder decoder;
    ASSERT_EQ(decoder.open(std::make_shared<MemorySource>(flac), native()), Decoder::OpenStatus::Ready);
    DecoderOptions mono;
    mono.channels = 1;
    Result<std::unique_ptr<Decoder>> copy = decoder.clone(mono);
    ASSERT_TRUE(copy.ok()) << copy.error().toString().toStdString();
    std::unique_ptr<Decoder> clone = copy.takeValue();
    EXPECT_EQ(clone->channels(), 1);
    ASSERT_EQ(clone->seek(44100), DecodeStatus::Ok);
    const Pcm part = readFrames(*clone, 100);
    EXPECT_EQ(part.frames(), 100);
    EXPECT_EQ(decoder.position(), 0); // independent

    Decoder growing;
    ASSERT_EQ(growing.open(std::make_shared<MemorySource>(flac, flac.size() / 2)), Decoder::OpenStatus::Ready);
    EXPECT_FALSE(growing.clone({}).ok());
}
