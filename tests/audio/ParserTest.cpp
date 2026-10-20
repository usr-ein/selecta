// SPDX-License-Identifier: GPL-3.0-or-later
// Selecta's own container parsers: MPEG frame headers, Xing/LAME tags, ID3v2 sizes, FLAC headers, WAV/AIFF chunks,
// and container detection. Files are synthetic (support/SyntheticAudio).
#include "SyntheticAudio.h"
#include "audio/ByteSource.h"
#include "audio/detail/ContainerProbe.h"
#include "audio/detail/Crc.h"
#include "audio/detail/FlacFormat.h"
#include "audio/detail/MpegAudio.h"
#include "audio/detail/PcmFormat.h"

#include <array>
#include <cmath>
#include <gtest/gtest.h>

using namespace selecta::audio;
using namespace selecta::audio::detail;
using namespace selecta::testing;

namespace {

constexpr int kMiB = 1024 * 1024;

ByteSpan span(const QByteArray &bytes)
{
    return {reinterpret_cast<const quint8 *>(bytes.constData()), static_cast<std::size_t>(bytes.size())};
}

ParseResult probe(const QByteArray &file, ProbeResult &result, qint64 available = -1)
{
    MemorySource source(file, available);
    SourceReader reader(source, source.snapshot());
    return probeContainer(reader, qint64{64} * kMiB, result);
}

} // namespace

TEST(MpegHeader, ParsesCommonHeaders)
{
    // MPEG-1 Layer III, 128 kbps, 44.1 kHz, joint stereo, no CRC.
    const std::optional<MpegHeader> h = parseMpegHeader(0xFFFB9064);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->version, 1);
    EXPECT_EQ(h->layer, 3);
    EXPECT_EQ(h->bitrateKbps, 128);
    EXPECT_EQ(h->sampleRate, 44100);
    EXPECT_EQ(h->channels, 2);
    EXPECT_EQ(h->frameBytes, 417);
    EXPECT_EQ(h->samplesPerFrame, 1152);
    EXPECT_FALSE(h->hasCrc);

    // MPEG-1 Layer III, 320 kbps, 48 kHz, padding.
    const std::optional<MpegHeader> p = parseMpegHeader(0xFFFBE600);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->bitrateKbps, 320);
    EXPECT_EQ(p->sampleRate, 48000);
    EXPECT_EQ(p->frameBytes, 961);

    // MPEG-2 Layer III, 64 kbps, 22.05 kHz, mono: 576 samples per frame.
    const std::optional<MpegHeader> m = parseMpegHeader(0xFFF380C0);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->version, 2);
    EXPECT_EQ(m->sampleRate, 22050);
    EXPECT_EQ(m->samplesPerFrame, 576);
    EXPECT_EQ(m->channels, 1);
    EXPECT_EQ(m->frameBytes, 72 * 64000 / 22050);
}

TEST(MpegHeader, RejectsInvalidFields)
{
    EXPECT_FALSE(parseMpegHeader(0x00000000));
    EXPECT_FALSE(parseMpegHeader(0xFFE80000)); // reserved version
    EXPECT_FALSE(parseMpegHeader(0xFFF90000)); // reserved layer (ADTS AAC looks like this)
    EXPECT_FALSE(parseMpegHeader(0xFFFBF000)); // bad bitrate index
    EXPECT_FALSE(parseMpegHeader(0xFFFB9C00)); // reserved sample rate
    const std::optional<MpegHeader> free = parseMpegHeader(0xFFFB0000);
    ASSERT_TRUE(free);
    EXPECT_EQ(free->frameBytes, 0); // free format: size unknown from the header
}

TEST(Id3v2, TagSizes)
{
    const QByteArray tag("ID3\x03\x00\x00\x00\x00\x02\x01", 10); // size 0x101 = 257
    EXPECT_EQ(id3v2TagSize(span(tag)), 10 + 257);
    const QByteArray footer("ID3\x04\x00\x10\x00\x00\x00\x10", 10);
    EXPECT_EQ(id3v2TagSize(span(footer)), 10 + 16 + 10);
    const QByteArray invalid("ID3\x03\x00\x00\x80\x00\x00\x00", 10); // not syncsafe
    EXPECT_EQ(id3v2TagSize(span(invalid)), -1);
    const QByteArray version("ID3\x09\x00\x00\x00\x00\x00\x00", 10);
    EXPECT_EQ(id3v2TagSize(span(version)), -1);
    EXPECT_EQ(id3v2TagSize(span(QByteArray("RIFF....WAVE"))), 0);
}

TEST(LameTag, ParsedFromALameEncodedFile)
{
    const Pcm pcm = sine(1000, 2.0);
    const QByteArray mp3 = makeMp3(pcm, {});
    const std::optional<MpegHeader> header = parseMpegHeader(be32(span(mp3), 0));
    ASSERT_TRUE(header);
    const std::optional<LameTag> tag =
        parseXingFrame(span(mp3).first(static_cast<std::size_t>(header->frameBytes)), *header);
    ASSERT_TRUE(tag);
    EXPECT_TRUE(tag->isInfo); // CBR
    EXPECT_TRUE(tag->encoder.startsWith(QStringLiteral("LAME")));
    EXPECT_TRUE(tag->crcValid);
    EXPECT_EQ(tag->lowpassHz, 20500); // LAME's 320 kbps lowpass
    EXPECT_GT(tag->encoderDelay, 0);
    EXPECT_GE(tag->encoderPadding, 0);
    // Gapless arithmetic: frames × 1152 − delay − padding = the source length.
    EXPECT_EQ((tag->frames * 1152) - tag->encoderDelay - tag->encoderPadding, pcm.frames());

    QByteArray corrupted = mp3;
    corrupted[60] =
        static_cast<char>(static_cast<quint8>(corrupted[60]) ^ 0x5AU); // inside the Xing fields, covered by the CRC
    const std::optional<LameTag> broken =
        parseXingFrame(span(corrupted).first(static_cast<std::size_t>(header->frameBytes)), *header);
    ASSERT_TRUE(broken);
    EXPECT_FALSE(broken->crcValid);
}

TEST(LameTag, AbsentWithoutTheInfoFrame)
{
    const QByteArray mp3 = makeMp3(sine(440, 1.0), {.lameTag = false});
    const std::optional<MpegHeader> header = parseMpegHeader(be32(span(mp3), 0));
    ASSERT_TRUE(header);
    EXPECT_FALSE(parseXingFrame(span(mp3).first(static_cast<std::size_t>(header->frameBytes)), *header));
}

TEST(Crc, KnownVectors)
{
    const QByteArray text("123456789");
    EXPECT_EQ(crc8Flac(span(text)), 0xF4);    // CRC-8/SMBUS (poly 0x07)
    EXPECT_EQ(crc16Flac(span(text)), 0xFEE8); // CRC-16/BUYPASS (poly 0x8005, not reflected)
    EXPECT_EQ(crc16Lame(span(text)), 0xBB3D); // CRC-16/ARC
}

TEST(FlacHeaders, StreamInfoAndFrames)
{
    const QByteArray flac = makeFlac(sine(1000, 1.0, 48000, 2), {.bits = 24});
    ASSERT_TRUE(flac.startsWith("fLaC"));
    const std::optional<FlacStreamInfo> info = parseFlacStreamInfo(span(flac).subspan(8, 34));
    ASSERT_TRUE(info);
    EXPECT_EQ(info->sampleRate, 48000);
    EXPECT_EQ(info->channels, 2);
    EXPECT_EQ(info->bitsPerSample, 24);
    EXPECT_EQ(info->totalSamples, 48000);
    // Find the first frame after the metadata and parse its header.
    qint64 pos = 4;
    bool last = false;
    while (!last) {
        last = (static_cast<quint8>(flac[static_cast<int>(pos)]) & 0x80U) != 0;
        pos += 4 + be24(span(flac), static_cast<std::size_t>(pos + 1));
    }
    const std::optional<FlacFrameHeader> frame =
        parseFlacFrameHeader(span(flac).subspan(static_cast<std::size_t>(pos), 16));
    ASSERT_TRUE(frame);
    EXPECT_TRUE(flacHeaderMatches(*frame, *info));
    EXPECT_EQ(flacFirstSample(*frame, frame->blockSize), 0);
    // Any flipped header bit breaks the CRC-8.
    QByteArray broken = flac.mid(static_cast<int>(pos), 16);
    broken[2] = static_cast<char>(static_cast<quint8>(broken[2]) ^ 0x01U);
    EXPECT_FALSE(parseFlacFrameHeader(span(broken)));
}

TEST(WavParser, ChunksBeforeDataAndExtensible)
{
    const Pcm pcm = sine(440, 0.1, 44100, 2);
    for (const WavOptions &options :
         {WavOptions{.bits = 16, .listChunkBytes = 3000}, WavOptions{.bits = 24, .extensible = true},
          WavOptions{.bits = 32, .floatingPoint = true}, WavOptions{.bits = 8}}) {
        const QByteArray wav = makeWav(pcm, options);
        MemorySource source(wav);
        SourceReader reader(source, source.snapshot());
        PcmLayout layout;
        const ParseResult r = parseWav(reader, kMiB, layout);
        ASSERT_TRUE(r.ok()) << r.message.toStdString();
        EXPECT_EQ(layout.channels, 2);
        EXPECT_EQ(layout.sampleRate, 44100);
        EXPECT_EQ(layout.frames(), pcm.frames());
        EXPECT_EQ(layout.encoding, options.floatingPoint
                                       ? PcmEncoding::Float
                                       : (options.bits == 8 ? PcmEncoding::UnsignedInt : PcmEncoding::SignedInt));
    }
}

TEST(WavParser, DataBeforeFormatNeedsTheCompleteFile)
{
    const QByteArray wav = makeWav(sine(440, 0.2), {.dataBeforeFormat = true});
    PcmLayout layout;
    MemorySource growing(wav, wav.size() - 10);
    SourceReader growingReader(growing, growing.snapshot());
    EXPECT_EQ(parseWav(growingReader, kMiB, layout).status, ParseStatus::NeedsCompleteFile);
    MemorySource complete(wav);
    SourceReader completeReader(complete, complete.snapshot());
    EXPECT_TRUE(parseWav(completeReader, kMiB, layout).ok());
}

TEST(WavParser, TruncatedAndHostileHeaders)
{
    const QByteArray wav = makeWav(sine(440, 0.2));
    PcmLayout layout;
    MemorySource prefix(wav, 20);
    SourceReader prefixReader(prefix, prefix.snapshot());
    EXPECT_EQ(parseWav(prefixReader, kMiB, layout).status, ParseStatus::NeedMoreData);
    MemorySource shortFile(wav.left(20));
    SourceReader shortReader(shortFile, shortFile.snapshot());
    EXPECT_EQ(parseWav(shortReader, kMiB, layout).status, ParseStatus::Corrupt);

    QByteArray zeroChannels = wav;
    zeroChannels[22] = 0;
    zeroChannels[23] = 0;
    MemorySource bad(zeroChannels);
    SourceReader badReader(bad, bad.snapshot());
    EXPECT_EQ(parseWav(badReader, kMiB, layout).status, ParseStatus::Corrupt);

    QByteArray rf64 = wav;
    rf64.replace(0, 4, "RF64");
    MemorySource rf(rf64);
    SourceReader rfReader(rf, rf.snapshot());
    EXPECT_EQ(parseWav(rfReader, kMiB, layout).status, ParseStatus::Unsupported);

    // A data chunk announcing more than the file holds: clamped and flagged.
    const QByteArray truncated = wav.left(wav.size() - 1000);
    MemorySource cut(truncated);
    SourceReader cutReader(cut, cut.snapshot());
    ASSERT_TRUE(parseWav(cutReader, kMiB, layout).ok());
    EXPECT_TRUE(layout.truncated);
    EXPECT_LE(layout.dataOffset + layout.dataBytes, truncated.size());

    // A huge chunk before data beyond the header bound.
    const QByteArray padded = makeWav(sine(440, 0.1), {.listChunkBytes = 300000});
    MemorySource big(padded);
    SourceReader bigReader(big, big.snapshot());
    EXPECT_EQ(parseWav(bigReader, 100000, layout).status, ParseStatus::TooLarge);
}

TEST(AiffParser, CommAfterSsndAndAifc)
{
    const Pcm pcm = sine(440, 0.2, 48000, 2);
    PcmLayout layout;
    const QByteArray late = makeAiff(pcm, {.commAfterSsnd = true});
    MemorySource growing(late, late.size() - 1);
    SourceReader growingReader(growing, growing.snapshot());
    EXPECT_EQ(parseAiff(growingReader, kMiB, layout).status, ParseStatus::NeedsCompleteFile);
    MemorySource complete(late);
    SourceReader completeReader(complete, complete.snapshot());
    ASSERT_TRUE(parseAiff(completeReader, kMiB, layout).ok());
    EXPECT_EQ(layout.sampleRate, 48000);
    EXPECT_EQ(layout.frames(), pcm.frames());
    EXPECT_TRUE(layout.bigEndian);

    const QByteArray sowt = makeAiff(pcm, {.bits = 16, .compression = "sowt"});
    MemorySource s(sowt);
    SourceReader sReader(s, s.snapshot());
    ASSERT_TRUE(parseAiff(sReader, kMiB, layout).ok());
    EXPECT_FALSE(layout.bigEndian);

    const QByteArray floats = makeAiff(pcm, {.compression = "fl32"});
    MemorySource f(floats);
    SourceReader fReader(f, f.snapshot());
    ASSERT_TRUE(parseAiff(fReader, kMiB, layout).ok());
    EXPECT_EQ(layout.encoding, PcmEncoding::Float);

    const QByteArray unknown = makeAiff(pcm, {.compression = "ima4"});
    MemorySource u(unknown);
    SourceReader uReader(u, u.snapshot());
    EXPECT_EQ(parseAiff(uReader, kMiB, layout).status, ParseStatus::Unsupported);
}

TEST(AiffParser, Extended80)
{
    const std::array<quint8, 10> rate44100 = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    EXPECT_DOUBLE_EQ(readExtended80(rate44100), 44100.0);
    const std::array<quint8, 10> negative = {0xC0, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(readExtended80(negative), 0.0);
    const std::array<quint8, 10> huge = {0x7F, 0xFE, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    const double absurd = readExtended80(huge); // a crafted exponent never yields a plausible rate
    EXPECT_FALSE(absurd >= 1.0 && absurd <= 768000.0);
}

TEST(ContainerProbe, RecognisesEveryFormat)
{
    const Pcm pcm = sine(440, 0.5);
    ProbeResult result;
    ASSERT_TRUE(probe(makeWav(pcm), result).ok());
    EXPECT_EQ(result.container, Container::Wav);
    ASSERT_TRUE(probe(makeAiff(pcm), result).ok());
    EXPECT_EQ(result.container, Container::Aiff);
    ASSERT_TRUE(probe(makeFlac(pcm), result).ok());
    EXPECT_EQ(result.container, Container::Flac);
    ASSERT_TRUE(probe(makeMp3(pcm), result).ok());
    EXPECT_EQ(result.container, Container::MpegAudio);
    EXPECT_EQ(result.streamStart, 0);
    ASSERT_TRUE(probe(makeOggFlac(pcm), result).ok());
    EXPECT_EQ(result.container, Container::Ogg);

    const QByteArray slow = makeM4a(pcm, {});
    ASSERT_TRUE(probe(slow, result).ok());
    EXPECT_EQ(result.container, Container::Mp4);
    EXPECT_EQ(result.mp4MoovEnd, -1); // mdat first

    const QByteArray fast = makeM4a(pcm, {.fastStart = true});
    ASSERT_TRUE(probe(fast, result).ok());
    EXPECT_EQ(result.container, Container::Mp4);
    EXPECT_GT(result.mp4MoovEnd, 0);
    EXPECT_LT(result.mp4MoovEnd, fast.size());
}

TEST(ContainerProbe, SkipsLargeId3AndJunkBeforeMpegAudio)
{
    const QByteArray mp3 = makeMp3(sine(440, 1.0), {.id3v2Bytes = 2 * kMiB, .junkBytes = 700});
    ProbeResult result;
    ASSERT_TRUE(probe(mp3, result).ok());
    EXPECT_EQ(result.container, Container::MpegAudio);
    EXPECT_EQ(result.leadingTagBytes, 2 * kMiB);
    EXPECT_EQ(result.streamStart, (2 * kMiB) + 700);

    // While the tag downloads, the probe waits instead of guessing from the picture bytes.
    EXPECT_EQ(probe(mp3, result, kMiB).status, ParseStatus::NeedMoreData);
    // A tag bigger than the bound is refused.
    MemorySource source(mp3);
    SourceReader reader(source, source.snapshot());
    EXPECT_EQ(probeContainer(reader, kMiB, result).status, ParseStatus::TooLarge);
}

TEST(ContainerProbe, UnknownAndTinyInputs)
{
    ProbeResult result;
    ASSERT_TRUE(probe(randomBytes(300000, 99), result).ok());
    EXPECT_EQ(result.container, Container::Unknown);
    EXPECT_EQ(probe(QByteArray(), result).status, ParseStatus::Corrupt);
    EXPECT_EQ(probe(QByteArray("ab"), result).status, ParseStatus::Corrupt);
    EXPECT_EQ(probe(makeWav(sine(440, 0.1)), result, 2).status, ParseStatus::NeedMoreData);
}
