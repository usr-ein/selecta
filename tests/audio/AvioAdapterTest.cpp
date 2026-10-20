// SPDX-License-Identifier: GPL-3.0-or-later
// The contract's AVIOContext test (audio §5.3, §12.3): FFmpeg's own demuxers, run through Selecta's AvioAdapter on a
// file that is still "downloading", never read or seek past the available bytes while the input is non-seekable. The
// control cases show the tail reads the same demuxers make when the input is seekable, which is why the adapter hides
// seekability until the file is complete.
#include "audio/detail/AvioAdapter.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/detail/Ffmpeg.h"

#include <gtest/gtest.h>
#include <vector>

using namespace selecta::audio;
using namespace selecta::audio::detail;
using namespace selecta::testing;

namespace {

struct Observation
{
    int packets = 0;
    std::vector<qint64> seekTargets; // absolute positions, AVSEEK_SIZE queries excluded
    qint64 highestRead = 0;          // end of the highest byte range returned to FFmpeg
    bool opened = false;
};

Observation demux(const std::shared_ptr<ByteSource> &source, const char *format, bool forceSeek = false)
{
    Observation observation;
    AvioAdapter avio(source);
    EXPECT_TRUE(avio.create().ok());
    avio.seekObserver = [&](qint64 offset, int whence) {
        if (hasFlag(whence, AVSEEK_SIZE)) {
            return;
        }
        const int mode = withoutFlag(whence, AVSEEK_FORCE);
        qint64 target = offset;
        if (mode == SEEK_CUR) {
            target = avio.position() + offset;
        } else if (mode == SEEK_END) {
            target = source->totalSize() + offset;
        }
        observation.seekTargets.push_back(target);
    };
    avio.readObserver = [&](qint64 position, int, int returned) {
        if (returned > 0) {
            observation.highestRead = std::max(observation.highestRead, position + returned);
        }
    };
    AVFormatContext *context = avformat_alloc_context();
    context->pb = avio.context();
    context->flags = withFlag(context->flags, AVFMT_FLAG_CUSTOM_IO);
    if (avformat_open_input(&context, "", av_find_input_format(format), nullptr) < 0) {
        return observation;
    }
    observation.opened = true;
    AVPacket *packet = av_packet_alloc();
    for (int i = 0; i < 100000 && av_read_frame(context, packet) >= 0; ++i) {
        ++observation.packets;
        av_packet_unref(packet);
    }
    if (forceSeek) {
        av_seek_frame(context, 0, 0, AVSEEK_FLAG_BACKWARD);
    }
    av_packet_free(&packet);
    avformat_close_input(&context);
    return observation;
}

void expectNothingPastAvailable(const QByteArray &file, const char *format, qint64 available)
{
    const auto source = std::make_shared<LoggingSource>(file, available);
    const Observation o = demux(source, format);
    ASSERT_TRUE(o.opened) << format;
    EXPECT_GT(o.packets, 0) << format << ": the prefix should already yield packets";
    EXPECT_EQ(source->violations(), 0) << format;
    EXPECT_LE(o.highestRead, available) << format;
    for (const qint64 target : o.seekTargets) {
        EXPECT_LE(target, available) << format << ": seek to " << target << " beyond the downloaded prefix";
    }
}

qint64 maxSeek(const Observation &o)
{
    qint64 highest = -1;
    for (const qint64 t : o.seekTargets) {
        highest = std::max(highest, t);
    }
    return highest;
}

} // namespace

TEST(AvioAdapter, NonSeekableMp3NeverReadsTheTail)
{
    const QByteArray mp3 = makeMp3(sine(440, 20.0), {.id3v1 = true});
    expectNothingPastAvailable(mp3, "mp3", mp3.size() / 3);
}

TEST(AvioAdapter, SeekableMp3LooksForId3v1AtTheEnd)
{
    // Control (§5.3): with a seekable input FFmpeg's MP3 demuxer seeks to filesize − 128 for an ID3v1 tag.
    const QByteArray mp3 = makeMp3(sine(440, 20.0), {.lameTag = false, .id3v1 = true});
    const Observation o = demux(std::make_shared<MemorySource>(mp3), "mp3");
    ASSERT_TRUE(o.opened);
    EXPECT_GE(maxSeek(o), mp3.size() - 128);
}

TEST(AvioAdapter, NonSeekableWavStopsAtTheData)
{
    const QByteArray wav = makeWav(sine(440, 10.0), {.listChunkBytes = 2000, .trailingChunkBytes = 5000});
    expectNothingPastAvailable(wav, "wav", wav.size() / 2);
}

TEST(AvioAdapter, SeekableWavParsesChunksAfterTheData)
{
    const QByteArray wav = makeWav(sine(440, 10.0), {.trailingChunkBytes = 5000});
    const Observation o = demux(std::make_shared<MemorySource>(wav), "wav");
    ASSERT_TRUE(o.opened);
    EXPECT_GE(maxSeek(o), wav.size() - 5000 - 8);
}

TEST(AvioAdapter, NonSeekableAiffStartsAtOnce)
{
    const QByteArray aiff = makeAiff(sine(440, 10.0), {.id3ChunkBytes = 3000});
    expectNothingPastAvailable(aiff, "aiff", aiff.size() / 2);
}

TEST(AvioAdapter, SeekableAiffSkipsTheSoundData)
{
    const QByteArray aiff = makeAiff(sine(440, 10.0));
    const Observation o = demux(std::make_shared<MemorySource>(aiff), "aiff");
    ASSERT_TRUE(o.opened);
    EXPECT_GE(maxSeek(o), aiff.size() - 16);
}

TEST(AvioAdapter, NonSeekableFlacAndFastStartMp4)
{
    const QByteArray flac = makeFlac(sine(440, 10.0), {.pictureBytes = 100000});
    expectNothingPastAvailable(flac, "flac", flac.size() / 2);
    const QByteArray m4a = makeM4a(sine(440, 10.0), {.fastStart = true});
    expectNothingPastAvailable(m4a, "mov", m4a.size() / 2);
}

TEST(AvioAdapter, StarvationIsNotStickyAndSizeIsReported)
{
    const QByteArray wav = makeWav(sine(440, 2.0));
    const auto source = std::make_shared<LoggingSource>(wav, 1000);
    AvioAdapter avio(source);
    ASSERT_TRUE(avio.create().ok());
    EXPECT_FALSE(avio.isSeekable());
    std::vector<quint8> buffer(4096);
    // Reads stop at the boundary with EOF and the starvation flag.
    EXPECT_EQ(avio_read(avio.context(), buffer.data(), 4096), 1000);
    EXPECT_TRUE(avio.takeStarved());
    EXPECT_EQ(avio_size(avio.context()), wav.size());
    // More bytes arrive: after a seek (which clears FFmpeg's EOF), reading resumes.
    source->setAvailable(3000);
    ASSERT_EQ(avio_seek(avio.context(), 1000, SEEK_SET), 1000);
    EXPECT_EQ(avio_read(avio.context(), buffer.data(), 2000), 2000);
    EXPECT_EQ(source->violations(), 0);
    source->setComplete();
    EXPECT_TRUE(avio.refreshSeekable());
}
