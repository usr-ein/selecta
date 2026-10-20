// SPDX-License-Identifier: GPL-3.0-or-later
// probeStreamInfo(): stream facts from the headers alone, for collection scans. Proves the read bound with a counting
// source and checks the facts against a full decode.
#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/Decoder.h"

#include <QTemporaryDir>

#include <atomic>
#include <cstring>
#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::audio;
using namespace selecta::testing;

namespace {

// A complete in-memory file that counts every byte read from it.
class CountingSource final : public ByteSource
{
public:
    explicit CountingSource(QByteArray data)
        : m_data(std::move(data))
    {}

    qint64 totalSize() const override { return m_data.size(); }
    SourceSnapshot snapshot() const override { return {m_data.size(), SourceState::Complete}; }
    qint64 read(qint64 position, char *destination, qint64 maxBytes) override
    {
        if (position < 0 || maxBytes < 0) {
            return -1;
        }
        const qint64 count = std::clamp<qint64>(m_data.size() - position, 0, maxBytes);
        if (count > 0) {
            std::memcpy(destination, m_data.constData() + position, static_cast<std::size_t>(count));
        }
        m_bytesRead += count;
        return count;
    }
    QString description() const override { return u"counting memory source"_s; }

    qint64 bytesRead() const { return m_bytesRead.load(); }

private:
    const QByteArray m_data;
    std::atomic<qint64> m_bytesRead{0};
};

struct Probed
{
    StreamInfo info;
    qint64 bytesRead = 0;
    qint64 fileBytes = 0;
};

Probed probe(const QByteArray &file)
{
    const auto source = std::make_shared<CountingSource>(file);
    const Result<StreamInfo> result = probeStreamInfo(source);
    if (!result.ok()) {
        ADD_FAILURE() << result.error().toString().toStdString();
        return {};
    }
    return {result.value(), source->bytesRead(), file.size()};
}

DecoderOptions native()
{
    DecoderOptions options;
    options.channels = 0;
    return options;
}

// Three minutes of broadband audio: large enough that reading it whole would stand out.
const Pcm &longPcm()
{
    static const Pcm pcm = [] {
        Pcm signal = tones({110, 1000, 7000}, 180.0, 44100, 2, 0.08);
        const Pcm noise = whiteNoise(180.0, 11, 44100, 2, 0.2);
        for (std::size_t i = 0; i < signal.samples.size(); ++i) {
            signal.samples[i] += noise.samples[i];
        }
        return quantised(signal, 16);
    }();
    return pcm;
}

} // namespace

TEST(StreamProbe, Mp3WithALameTagReadsOnlyTheStart)
{
    // A big ID3v2 tag (artwork) in front: skipped by its size, never read.
    const QByteArray mp3 = makeMp3(longPcm(), {.id3v2Bytes = 2 * 1024 * 1024});
    const Probed probed = probe(mp3);
    EXPECT_LT(probed.bytesRead, 96 * 1024) << probed.bytesRead << " of " << probed.fileBytes; // ~82 KB of 9 MB
    EXPECT_LT(probed.bytesRead * 50, probed.fileBytes);                                       // under 2 %
    const StreamInfo &info = probed.info;
    EXPECT_EQ(info.container, Container::MpegAudio);
    EXPECT_EQ(info.codec, Codec::Mp3);
    EXPECT_EQ(info.sampleRate, 44100);
    EXPECT_EQ(info.channels, 2);
    EXPECT_TRUE(info.exactLength); // the LAME tag's frame count, delay and padding
    EXPECT_EQ(info.totalFrames, longPcm().frames());
    EXPECT_EQ(info.bitrateKbps, 320);
    ASSERT_TRUE(info.lameTag.has_value());
    EXPECT_TRUE(info.lameTag->crcValid);
    EXPECT_FALSE(info.mpeg.has_value());                   // nothing indexed
    EXPECT_EQ(info.audioOffset, (2 * 1024 * 1024) + 1044); // after the tag and the Info frame (320 kbps, unpadded)
}

TEST(StreamProbe, Mp3WithoutATagIsEstimated)
{
    const QByteArray mp3 = makeMp3(longPcm(), {.lameTag = false, .id3v1 = true});
    const Probed probed = probe(mp3);
    EXPECT_LT(probed.bytesRead * 50, probed.fileBytes);
    EXPECT_FALSE(probed.info.exactLength);
    // CBR: the estimate from the size and the first frame is within a frame or two of the decoded length.
    const Decoded full = decodeAll(mp3, native());
    EXPECT_NEAR(static_cast<double>(probed.info.totalFrames), static_cast<double>(full.pcm.frames()), 2 * 1152.0);
    EXPECT_EQ(probed.info.bitrateKbps, 320);

    // VBR with a Xing tag: exact length, average bitrate from the tag's byte count.
    const QByteArray vbr = makeMp3(longPcm(), {.vbr = true, .vbrQuality = 2});
    const Probed vbrProbed = probe(vbr);
    EXPECT_LT(vbrProbed.bytesRead * 30, vbrProbed.fileBytes);
    EXPECT_TRUE(vbrProbed.info.exactLength);
    EXPECT_EQ(vbrProbed.info.totalFrames, longPcm().frames());
    const Decoded vbrFull = decodeAll(vbr, native());
    EXPECT_NEAR(vbrProbed.info.bitrateKbps, vbrFull.info.bitrateKbps, 3);
}

TEST(StreamProbe, FlacReadsMetadataOnly)
{
    const QByteArray flac = makeFlac(longPcm(), {.pictureBytes = 1024 * 1024});
    const Probed probed = probe(flac);
    EXPECT_LT(probed.bytesRead, 48 * 1024) << probed.bytesRead << " of " << probed.fileBytes; // ~33 KB of 29 MB
    EXPECT_EQ(probed.info.container, Container::Flac);
    EXPECT_TRUE(probed.info.lossless);
    EXPECT_EQ(probed.info.bitsPerSample, 16);
    EXPECT_TRUE(probed.info.exactLength); // STREAMINFO's total
    EXPECT_EQ(probed.info.totalFrames, longPcm().frames());
    const Decoded full = decodeAll(flac, native());
    EXPECT_NEAR(probed.info.bitrateKbps, full.info.bitrateKbps, 2); // from the file size
}

TEST(StreamProbe, PcmReadsChunkHeaders)
{
    const QByteArray wav = makeWav(longPcm(), {.bits = 24, .listChunkBytes = 4096});
    const Probed probed = probe(wav);
    EXPECT_LT(probed.bytesRead, 4096); // chunk headers: under 100 bytes of 47 MB
    EXPECT_EQ(probed.info.container, Container::Wav);
    EXPECT_EQ(probed.info.bitsPerSample, 24);
    EXPECT_TRUE(probed.info.exactLength);
    EXPECT_EQ(probed.info.totalFrames, longPcm().frames());

    const QByteArray aiff = makeAiff(longPcm());
    const Probed aiffProbed = probe(aiff);
    EXPECT_LT(aiffProbed.bytesRead, 4096);
    EXPECT_EQ(aiffProbed.info.container, Container::Aiff);
    EXPECT_EQ(aiffProbed.info.totalFrames, longPcm().frames());
}

TEST(StreamProbe, FfmpegFormatsAreOpenedButNeverDecoded)
{
    // MP4 with its index at the end: the demuxer reads the `moov`, not the audio.
    const Pcm pcm = quantised(tones({220, 3000}, 60.0, 44100, 2, 0.3), 16);
    const QByteArray m4a = makeM4a(pcm, {.alac = true});
    const Probed probed = probe(m4a);
    EXPECT_LT(probed.bytesRead, 128 * 1024) << probed.bytesRead << " of " << probed.fileBytes; // ~69 KB of 2.9 MB
    EXPECT_EQ(probed.info.container, Container::Mp4);
    EXPECT_EQ(probed.info.codec, Codec::Alac);
    EXPECT_EQ(probed.info.bitsPerSample, 16);
    EXPECT_TRUE(probed.info.exactLength);
    EXPECT_NEAR(static_cast<double>(probed.info.totalFrames), static_cast<double>(pcm.frames()), 4096.0);

    const QByteArray ogg = makeOggFlac(pcm);
    const Probed oggProbed = probe(ogg);
    EXPECT_EQ(oggProbed.info.container, Container::Ogg);
    EXPECT_EQ(oggProbed.info.sampleRate, 44100);
    // FFmpeg's Ogg demuxer reads its probe and the last pages (for the length): ~196 KB here, whatever the size.
    EXPECT_LT(oggProbed.bytesRead, 256 * 1024) << oggProbed.bytesRead << " of " << oggProbed.fileBytes;
}

TEST(StreamProbe, FilesAndFailures)
{
    const QTemporaryDir dir;
    const QString path = dir.filePath(u"probe.flac"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray flac = makeFlac(quantised(sine(440, 2.0), 16));
    ASSERT_EQ(file.write(flac), flac.size());
    file.close();
    const Result<StreamInfo> fromPath = probeStreamInfo(path);
    ASSERT_TRUE(fromPath.ok());
    EXPECT_EQ(fromPath.value().totalFrames, 88200);

    EXPECT_EQ(probeStreamInfo(dir.filePath(u"missing.mp3"_s)).error().code, ErrorCode::OpenFailed);
    EXPECT_EQ(probeStreamInfo(std::shared_ptr<ByteSource>()).error().code, ErrorCode::InvalidArgument);
    {
        const Result<StreamInfo> noise = probeStreamInfo(std::make_shared<MemorySource>(randomBytes(4096, 9)));
        EXPECT_FALSE(noise.ok()) << (noise.ok() ? containerName(noise.value().container).toStdString() + " " +
                                                      noise.value().codecName.toStdString() + " " +
                                                      std::to_string(noise.value().sampleRate)
                                                : std::string());
    }
    EXPECT_FALSE(probeStreamInfo(std::make_shared<MemorySource>(flac.left(20))).ok()); // ends inside STREAMINFO
    // A decoder for the same file still works normally: probing changes nothing global.
    EXPECT_EQ(decodeAll(flac, native()).pcm.frames(), 88200);
}
