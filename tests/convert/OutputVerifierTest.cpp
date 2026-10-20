// SPDX-License-Identifier: GPL-3.0-or-later
// C2: the verifier finds what is wrong with an output (stream facts, length, content, MP3 frames and LAME tag, tags,
// cover) and the PCM digest identifies audio across containers.
#include "convert/OutputVerifier.h"

#include "ConvertTestUtil.h"
#include "SyntheticAudio.h"
#include "convert/TagWriter.h"

#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;
using selecta::audio::CancellationFlag;
using selecta::audio::Container;
using selecta::audio::ErrorCode;

namespace {

bool mentions(const QStringList &problems, const QString &text)
{
    return std::ranges::any_of(problems, [&text](const QString &problem) { return problem.contains(text); });
}

std::string listed(const QStringList &problems)
{
    return problems.join(u"\n"_s).toStdString();
}

class Verifying : public ::testing::Test
{
protected:
    QTemporaryDir dir;
    QString write(const QString &name, const QByteArray &bytes) const { return writeFile(dir.path(), name, bytes); }
};

} // namespace

TEST_F(Verifying, AGoodFilePasses)
{
    const Pcm pcm = quantised(sine(440.0, 1.0), 16);
    const QString path = write(u"good.mp3"_s, makeMp3(pcm));
    VerifyExpectations expectations;
    expectations.container = Container::MpegAudio;
    expectations.sampleRate = 44100;
    expectations.channels = 2;
    expectations.expectedFrames = pcm.frames();
    expectations.mp3CbrKbps = 320;
    expectations.requireLameTag = true;
    const Result<VerifyReport> report = verifyOutput(path, expectations);
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.value().passed()) << listed(report.value().problems);
    EXPECT_EQ(report.value().decodedFrames, pcm.frames());
    EXPECT_EQ(report.value().pcmDigest.size(), 16); // MD5
    EXPECT_EQ(report.value().pcmDigest, pcmDigest(path).value());
}

TEST_F(Verifying, StreamFactsLengthAndContentAreChecked)
{
    const Pcm pcm = quantised(sine(440.0, 1.0), 16);
    const QString path = write(u"facts.mp3"_s, makeMp3(pcm, {.bitrateKbps = 256}));
    VerifyExpectations wrong;
    wrong.container = Container::Flac;
    wrong.sampleRate = 48000;
    wrong.channels = 1;
    wrong.expectedFrames = pcm.frames() + 5000;
    wrong.frameTolerance = 1152;
    wrong.expectedPcmDigest = QByteArray(16, 'x');
    wrong.mp3CbrKbps = 320;
    const Result<VerifyReport> report = verifyOutput(path, wrong);
    ASSERT_TRUE(report.ok());
    const QStringList &problems = report.value().problems;
    EXPECT_TRUE(mentions(problems, u"expected FLAC"_s)) << listed(problems);
    EXPECT_TRUE(mentions(problems, u"expected 48000 Hz"_s));
    EXPECT_TRUE(mentions(problems, u"expected 1"_s));
    EXPECT_TRUE(mentions(problems, u"frames, expected"_s));
    EXPECT_TRUE(mentions(problems, u"isn't identical"_s));
    EXPECT_TRUE(mentions(problems, u"expected all at 320 kbps"_s));
}

TEST_F(Verifying, AnMp3WithoutAGenuineLameTagFails)
{
    const Pcm pcm = quantised(sine(440.0, 1.0), 16);
    const QString bare = write(u"bare.mp3"_s, makeMp3(pcm, {.lameTag = false}));
    VerifyExpectations expectations;
    expectations.requireLameTag = true;
    const Result<VerifyReport> report = verifyOutput(bare, expectations);
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(mentions(report.value().problems, u"no Info/Xing header"_s)) << listed(report.value().problems);

    // A LAME tag whose CRC no longer matches (one byte of the tag changed): DJ apps would decode it as audio.
    QByteArray mp3 = makeMp3(pcm);
    const qsizetype lame = mp3.indexOf("LAME");
    ASSERT_GT(lame, 0);
    mp3[lame + 12] = static_cast<char>(static_cast<quint8>(mp3[lame + 12]) ^ 0x01U); // inside the CRC's 190 bytes
    const QString broken = write(u"broken.mp3"_s, mp3);
    const Result<VerifyReport> crc = verifyOutput(broken, expectations);
    ASSERT_TRUE(crc.ok());
    EXPECT_TRUE(mentions(crc.value().problems, u"CRC"_s)) << listed(crc.value().problems);
}

TEST_F(Verifying, DamageIsAProblemUnlessExcused)
{
    const QByteArray flac = makeFlac(quantised(sine(440.0, 2.0), 16));
    const QString path = write(u"cut.flac"_s, flac.left(flac.size() / 2));
    const Result<VerifyReport> strict = verifyOutput(path, {});
    ASSERT_TRUE(strict.ok());
    EXPECT_TRUE(strict.value().stats.truncated);
    EXPECT_TRUE(mentions(strict.value().problems, u"ends before"_s)) << listed(strict.value().problems);
    VerifyExpectations lenient;
    lenient.requireCleanDecode = false;
    EXPECT_TRUE(verifyOutput(path, lenient).value().passed());
}

TEST_F(Verifying, TagsMustReadBackExactlyAndTheCoverMustDecode)
{
    const QString path = write(u"tagged.wav"_s, makeWav(quantised(sine(440.0, 0.5), 16)));
    const MetadataModel model = richModel();
    ASSERT_TRUE(writeTags(path, model).ok());
    VerifyExpectations expectations;
    expectations.expectedTags = tagsAsWritten(model, Container::Wav);
    expectations.expectFrontCover = true;
    expectations.expectedCoverSize = QSize(300, 300);
    const Result<VerifyReport> good = verifyOutput(path, expectations);
    ASSERT_TRUE(good.ok());
    EXPECT_TRUE(good.value().passed()) << listed(good.value().problems);

    MetadataModel other = *expectations.expectedTags;
    other.set(keys::Title, u"Another Title"_s, Provenance::Release);
    other.set(u"NOTWRITTEN"_s, u"x"_s, Provenance::Release);
    other.remove(keys::Album);
    VerifyExpectations wrong = expectations;
    wrong.expectedTags = other;
    wrong.expectedCoverSize = QSize(600, 600);
    const Result<VerifyReport> bad = verifyOutput(path, wrong);
    ASSERT_TRUE(bad.ok());
    const QStringList &problems = bad.value().problems;
    EXPECT_TRUE(mentions(problems, u"tag TITLE reads back as"_s)) << listed(problems);
    EXPECT_TRUE(mentions(problems, u"tag NOTWRITTEN is missing"_s));
    EXPECT_TRUE(mentions(problems, u"unexpected tag ALBUM"_s));
    EXPECT_TRUE(mentions(problems, u"expected 600×600"_s));
    EXPECT_TRUE(mentions(problems, u"RIFF INFO title"_s)); // INFO says the written title, not "Another Title"

    // No cover at all.
    const QString bare = write(u"bare.wav"_s, makeWav(quantised(sine(440.0, 0.5), 16)));
    MetadataModel noCover = model;
    noCover.clearPictures();
    ASSERT_TRUE(writeTags(bare, noCover).ok());
    VerifyExpectations wantsCover;
    wantsCover.expectFrontCover = true;
    EXPECT_TRUE(mentions(verifyOutput(bare, wantsCover).value().problems, u"0 embedded pictures"_s));
}

TEST_F(Verifying, ThePcmDigestIdentifiesAudioAcrossContainers)
{
    const Pcm pcm = quantised(tones({300.0, 3000.0}, 1.0, 44100, 2, 0.3), 16);
    const Result<QByteArray> wav = pcmDigest(write(u"a.wav"_s, makeWav(pcm)));
    const Result<QByteArray> flac = pcmDigest(write(u"a.flac"_s, makeFlac(pcm)));
    const Result<QByteArray> aiff = pcmDigest(write(u"a.aiff"_s, makeAiff(pcm)));
    ASSERT_TRUE(wav.ok() && flac.ok() && aiff.ok());
    EXPECT_EQ(wav.value(), flac.value());
    EXPECT_EQ(wav.value(), aiff.value());
    const Result<QByteArray> other = pcmDigest(write(u"b.wav"_s, makeWav(quantised(sine(301.0, 1.0), 16))));
    EXPECT_NE(other.value(), wav.value());
}

TEST_F(Verifying, ErrorsAreNotFailedChecks)
{
    Result<VerifyReport> report = verifyOutput(dir.filePath(u"missing.flac"_s), {});
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::OpenFailed);
    report = verifyOutput(write(u"noise.flac"_s, randomBytes(4096, 3)), {});
    EXPECT_FALSE(report.ok());
    CancellationFlag cancel;
    cancel.cancel();
    report = verifyOutput(write(u"long.wav"_s, makeWav(quantised(sine(440.0, 1.0), 16))), {}, &cancel);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.error().code, ErrorCode::Cancelled);
    EXPECT_FALSE(pcmDigest(dir.filePath(u"missing.wav"_s)).ok());
}
