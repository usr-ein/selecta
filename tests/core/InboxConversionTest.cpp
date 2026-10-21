// SPDX-License-Identifier: GPL-3.0-or-later
// P-025: what the Inbox makes of each source format, and real Opus, Vorbis and AAC files written as tagged MP3 320.
#include "core/InboxConversion.h"

#include "SyntheticAudio.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::audio::Codec;
using selecta::audio::Container;
using selecta::convert::SourceQuality;
using selecta::core::InboxFormat;
using selecta::core::inboxFormatFor;
namespace convert = selecta::convert;
namespace keys = selecta::convert::keys;
namespace synth = selecta::testing;

namespace {

SourceQuality lossy(Codec codec, Container container, int kbps, int rate = 44100)
{
    SourceQuality source;
    source.container = container;
    source.codec = codec;
    source.sampleRate = rate;
    source.channels = 2;
    source.bitrateKbps = kbps;
    source.constantBitrate = codec == Codec::Mp3;
    return source;
}

SourceQuality lossless(Codec codec, Container container, int rate)
{
    SourceQuality source;
    source.container = container;
    source.codec = codec;
    source.lossless = true;
    source.bitDepth = 16;
    source.effectiveBitDepth = 16;
    source.sampleRate = rate;
    source.channels = 2;
    source.bitrateKbps = 700;
    return source;
}

void expectMp3_320(const InboxFormat &format, const char *what)
{
    EXPECT_FALSE(format.kept) << what;
    EXPECT_FALSE(format.belowFormat) << what;
    EXPECT_EQ(format.settings.format, convert::OutputFormat::Mp3) << what;
    EXPECT_EQ(format.settings.mp3.mode, convert::Mp3Mode::Cbr) << what;
    EXPECT_EQ(format.settings.mp3.bitrateKbps, 320) << what;
    EXPECT_EQ(format.extension, u"mp3"_s) << what;
    EXPECT_EQ(format.format, u"MP3"_s) << what;
    EXPECT_EQ(format.bitrate, 320) << what;
}

QByteArray jpeg(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(QColor(30, 120, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return bytes;
}

QString write(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

convert::MetadataModel releaseTags()
{
    convert::MetadataModel tags;
    tags.set(keys::Title, u"Switchup (WIP)"_s, convert::Provenance::Release);
    tags.set(keys::Artist, u"nohpetS"_s, convert::Provenance::Release);
    tags.set(keys::Album, u"Sponge"_s, convert::Provenance::Release);
    tags.set(keys::Label, u"Tidal Records"_s, convert::Provenance::Release);
    tags.set(keys::Isrc, u"QZFZ62317216"_s, convert::Provenance::Release);
    return tags;
}

// The source converted for the Inbox with the release's tags and a 1200 px cover; checks the result is a tagged
// MP3 320 CBR at `rate` with the cover scaled for CDJs, and that the source wasn't touched.
void expectTaggedMp3(const QString &source, const QString &suffix, int rate)
{
    const QByteArray before = contents(source);
    const convert::Result<SourceQuality> probed = convert::probeSourceQuality(source, false);
    ASSERT_TRUE(probed.ok()) << probed.error().message.toStdString();
    const InboxFormat format = inboxFormatFor(probed.value(), suffix);
    expectMp3_320(format, suffix.toUtf8().constData());

    convert::MetadataModel tags = releaseTags();
    QString why;
    selecta::core::attachCover(tags, jpeg(1200), &why);
    EXPECT_TRUE(why.isEmpty()) << why.toStdString();
    const QString output = QFileInfo(source).absoluteDir().filePath(u"out-"_s + suffix + u".mp3"_s);
    const convert::Result<convert::TranscodeReport> report =
        selecta::core::writeInboxFile(source, output, format, tags, u"Selecta 9.9.9"_s, nullptr);
    ASSERT_TRUE(report.ok()) << report.error().message.toStdString();
    EXPECT_NEAR(report.value().source.durationSeconds(), 4.0, 0.1);

    const convert::Result<SourceQuality> written = convert::probeSourceQuality(output, false);
    ASSERT_TRUE(written.ok());
    EXPECT_EQ(written.value().codec, Codec::Mp3);
    EXPECT_EQ(written.value().bitrateKbps, 320);
    EXPECT_TRUE(written.value().constantBitrate);
    EXPECT_EQ(written.value().sampleRate, rate);
    const convert::Result<convert::MetadataModel> read = convert::readTags(output);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().value(keys::Title), u"Switchup (WIP)"_s);
    EXPECT_EQ(read.value().value(keys::Artist), u"nohpetS"_s);
    EXPECT_EQ(read.value().value(keys::Label), u"Tidal Records"_s);
    EXPECT_EQ(read.value().value(keys::Isrc), u"QZFZ62317216"_s);
    EXPECT_EQ(read.value().value(keys::EncodedBy), u"Selecta 9.9.9"_s);
    ASSERT_TRUE(read.value().frontCover().has_value());
    EXPECT_EQ(QImage::fromData(read.value().frontCover()->data).width(), 800);
    EXPECT_EQ(contents(source), before); // the source is only read: never tagged
}

} // namespace

TEST(InboxFormat, EveryLossyFormatButMp3BecomesMp3_320)
{
    expectMp3_320(inboxFormatFor(lossy(Codec::Opus, Container::Ogg, 160, 48000), u"opus"_s), "Opus");
    expectMp3_320(inboxFormatFor(lossy(Codec::Opus, Container::Ogg, 64, 48000), u"opus"_s), "Opus 64");
    expectMp3_320(inboxFormatFor(lossy(Codec::Vorbis, Container::Ogg, 192), u"ogg"_s), "Vorbis");
    expectMp3_320(inboxFormatFor(lossy(Codec::Vorbis, Container::Ogg, 96, 22050), u"ogg"_s), "Vorbis 22.05 kHz");
    expectMp3_320(inboxFormatFor(lossy(Codec::Aac, Container::Mp4, 256), u"m4a"_s), "AAC");
    expectMp3_320(inboxFormatFor(lossy(Codec::Aac, Container::Mp4, 320), u"M4A"_s), "AAC 320");
    expectMp3_320(inboxFormatFor(lossy(Codec::Mp2, Container::MpegAudio, 256), u"mp2"_s), "MP2");
    SourceQuality wma = lossy(Codec::Other, Container::Other, 192);
    wma.codecName = u"wmav2"_s;
    expectMp3_320(inboxFormatFor(wma, u"wma"_s), "WMA");
}

TEST(InboxFormat, Mp3sAreKeptAsTheyAre)
{
    const InboxFormat top = inboxFormatFor(lossy(Codec::Mp3, Container::MpegAudio, 320), u"MP3"_s);
    EXPECT_TRUE(top.kept);
    EXPECT_FALSE(top.belowFormat);
    EXPECT_EQ(top.settings.format, convert::OutputFormat::KeepAudio);
    EXPECT_EQ(top.extension, u"mp3"_s);
    EXPECT_EQ(top.bitrate, 320);

    const InboxFormat below = inboxFormatFor(lossy(Codec::Mp3, Container::MpegAudio, 192), u"mp3"_s);
    EXPECT_TRUE(below.kept);
    EXPECT_TRUE(below.belowFormat);
    EXPECT_EQ(below.format, u"MP3"_s);
    EXPECT_EQ(below.bitrate, 192);
}

TEST(InboxFormat, OnlyAFormatSelectaCanTagIsKept)
{
    // A lossless file the advisor won't make an MP3 unasked (below 44.1 kHz) is kept when Selecta can tag it...
    const InboxFormat flac = inboxFormatFor(lossless(Codec::Flac, Container::Flac, 22050), u"flac"_s);
    EXPECT_TRUE(flac.kept);
    EXPECT_EQ(flac.extension, u"flac"_s);
    EXPECT_EQ(flac.format, u"FLAC"_s);
    // ...and converted when it can't: kept, it could never reach the Inbox tagged.
    SourceQuality ape = lossless(Codec::Other, Container::Other, 22050);
    ape.codecName = u"ape"_s;
    expectMp3_320(inboxFormatFor(ape, u"ape"_s), "APE 22.05 kHz");
    expectMp3_320(inboxFormatFor(lossless(Codec::Flac, Container::Ogg, 22050), u"oga"_s), "Ogg FLAC 22.05 kHz");
    // A genuine lossless file becomes MP3 320 as before.
    expectMp3_320(inboxFormatFor(lossless(Codec::Flac, Container::Flac, 44100), u"flac"_s), "FLAC");
}

// The failure in the maintainer's note ("Conversion failed: Selecta writes tags only to MP3, AIFF, WAV, FLAC and
// M4A files"): an Opus download was kept as it was, and its tags couldn't be written.
TEST(InboxConversion, AnOpusFileBecomesATaggedMp3_320)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source =
        write(dir, u"00 - 2a_foundU_switchup_WIP (_leg!_).opus"_s, synth::makeOpus(synth::sine(440.0, 4.0, 48000)));
    ASSERT_FALSE(source.isEmpty());
    expectTaggedMp3(source, u"opus"_s, 48000);
}

TEST(InboxConversion, AnOggVorbisFileBecomesATaggedMp3_320)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = write(dir, u"track.ogg"_s, synth::makeOggVorbis(synth::sine(440.0, 4.0, 44100)));
    ASSERT_FALSE(source.isEmpty());
    expectTaggedMp3(source, u"ogg"_s, 44100);
}

TEST(InboxConversion, AnAacFileBecomesATaggedMp3_320)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = write(dir, u"track.m4a"_s, synth::makeM4a(synth::sine(440.0, 4.0, 44100)));
    ASSERT_FALSE(source.isEmpty());
    expectTaggedMp3(source, u"m4a"_s, 44100);
}
