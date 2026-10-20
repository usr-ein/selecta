// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ConverterService.h"

#include "SyntheticAudio.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::ConverterItem;
using selecta::core::ConverterOptions;
using selecta::core::ConverterService;
using State = ConverterItem::State;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray jpeg(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(QColor(120, 60, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return bytes;
}

class ConverterServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ConverterService::Options options;
        options.stagingDir = dir.filePath(u"staging"_s);
        options.encodedBy = u"Selecta 9.9.9"_s;
        options.maxConcurrent = 2;
        service = std::make_unique<ConverterService>(options);
    }

    QString album(const QString &file) const { return dir.filePath(u"Okuma Drift - Night Signals (2024)/"_s + file); }

    // Waits until no item is reading or converting, and returns them.
    QList<ConverterItem> settled()
    {
        EXPECT_TRUE(QTest::qWaitFor(
            [this] {
                service->waitForIdle();
                const QList<ConverterItem> items = service->items();
                return std::ranges::none_of(items, [](const ConverterItem &item) {
                    return item.state == State::Reading || item.state == State::Converting;
                });
            },
            60'000));
        return service->items();
    }

    QTemporaryDir dir;
    std::unique_ptr<ConverterService> service;
};

} // namespace

TEST_F(ConverterServiceTest, AFlacBecomesATaggedMp3NextToIt)
{
    const QString source = album(u"01 - Neon Rain.flac"_s);
    const QByteArray original = synth::makeFlac(synth::quantised(synth::sine(440.0, 3.0), 16));
    ASSERT_TRUE(writeFile(source, original));
    ASSERT_TRUE(writeFile(album(u"cover.jpg"_s), jpeg(1200)));

    service->add({source});
    QList<ConverterItem> items = settled();
    ASSERT_EQ(items.size(), 1);
    const ConverterItem &ready = items.constFirst();
    ASSERT_EQ(ready.state, State::Ready) << ready.message.toStdString();
    EXPECT_TRUE(ready.quality.startsWith(u"FLAC"_s)) << ready.quality.toStdString();
    EXPECT_TRUE(ready.verdict.startsWith(u"MP3 320"_s)) << ready.verdict.toStdString();
    EXPECT_FALSE(ready.askFirst);
    EXPECT_TRUE(ready.isConvertible());
    // Its folder and name say what its (missing) tags don't (CONV-5).
    EXPECT_EQ(ready.tags.value(keys::Title), u"Neon Rain"_s);
    EXPECT_EQ(ready.tags.value(keys::Artist), u"Okuma Drift"_s);
    EXPECT_EQ(ready.tags.value(keys::Album), u"Night Signals"_s);
    EXPECT_EQ(ready.tags.value(keys::TrackNumber), u"1"_s);
    EXPECT_EQ(ready.tags.value(keys::Date), u"2024"_s);
    EXPECT_TRUE(ready.added.contains(u"TITLE"_s));
    EXPECT_EQ(ready.cover, u"cover.jpg from the folder"_s);

    const QSignalSpy finished(service.get(), &ConverterService::finished);
    service->start();
    items = settled();
    const ConverterItem &done = items.constFirst();
    ASSERT_EQ(done.state, State::Done) << done.message.toStdString();
    EXPECT_EQ(QFileInfo(done.outputPath).canonicalFilePath(),
              QFileInfo(album(u"01 - Neon Rain.mp3"_s)).canonicalFilePath());
    ASSERT_EQ(finished.size(), 1);
    EXPECT_EQ(finished.at(0).at(0).toInt(), 1);
    EXPECT_EQ(finished.at(0).at(1).toInt(), 0);

    // MP3 320 with the tags and the folder's cover, sized for CDJs; the original untouched (CONV-3, CONV-7).
    const auto quality = selecta::convert::probeSourceQuality(done.outputPath, false);
    ASSERT_TRUE(quality.ok());
    EXPECT_EQ(quality.value().bitrateKbps, 320);
    const auto tags = selecta::convert::readTags(done.outputPath);
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().value(keys::Title), u"Neon Rain"_s);
    EXPECT_EQ(tags.value().value(keys::Album), u"Night Signals"_s);
    EXPECT_EQ(tags.value().value(keys::EncodedBy), u"Selecta 9.9.9"_s);
    ASSERT_TRUE(tags.value().frontCover().has_value());
    EXPECT_EQ(QImage::fromData(tags.value().frontCover()->data).width(), 800);
    EXPECT_EQ(readFile(source), original);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"staging/converter/1"_s)));
}

TEST_F(ConverterServiceTest, ANameAlreadyTakenGetsANumberAndTheFolderCanBeChosen)
{
    const QString source = album(u"01 - Neon Rain.flac"_s);
    ASSERT_TRUE(writeFile(source, synth::makeFlac(synth::quantised(synth::sine(440.0, 1.0), 16))));
    ASSERT_TRUE(writeFile(album(u"01 - Neon Rain.mp3"_s), "taken"));
    service->add({source});
    settled();
    service->start();
    const ConverterItem first = settled().constFirst();
    ASSERT_EQ(first.state, State::Done) << first.message.toStdString();
    EXPECT_TRUE(first.outputPath.endsWith(u"01 - Neon Rain (2).mp3"_s)) << first.outputPath.toStdString();
    EXPECT_EQ(readFile(album(u"01 - Neon Rain.mp3"_s)), QByteArray("taken")); // never replaced

    ConverterOptions options;
    options.target.format = selecta::convert::OutputFormat::Aiff;
    options.outputFolder = dir.filePath(u"Out"_s);
    ASSERT_TRUE(QDir().mkpath(options.outputFolder));
    service->setOptions(options);
    service->add({source}); // the first is done: it can be added again
    settled();
    service->start();
    const ConverterItem second = settled().constLast();
    ASSERT_EQ(second.state, State::Done) << second.message.toStdString();
    EXPECT_EQ(QFileInfo(second.outputPath).absolutePath(), QFileInfo(options.outputFolder).absoluteFilePath());
    EXPECT_TRUE(second.outputPath.endsWith(u".aiff"_s)) << second.outputPath.toStdString();
}

TEST_F(ConverterServiceTest, ALossyFileAsksBeforeConvertingWithoutGain)
{
    const QString source = album(u"02 - Low Tide.mp3"_s);
    ASSERT_TRUE(writeFile(source, synth::makeMp3(synth::sine(330.0, 2.0), {.bitrateKbps = 192})));
    service->add({source});
    ConverterItem item = settled().constFirst();
    ASSERT_EQ(item.state, State::Ready) << item.message.toStdString();
    EXPECT_TRUE(item.askFirst);
    EXPECT_FALSE(item.isConvertible());
    EXPECT_FALSE(item.notices.isEmpty());
    EXPECT_FALSE(item.verdict.isEmpty());

    service->start(); // nothing to do yet
    EXPECT_EQ(service->items().constFirst().state, State::Ready);
    service->confirm(item.id, true);
    service->start();
    item = settled().constFirst();
    EXPECT_EQ(item.state, State::Done) << item.message.toStdString();
}

TEST_F(ConverterServiceTest, AFileThatIsntAudioFailsAndCancellingStopsAConversion)
{
    const QString broken = album(u"broken.flac"_s);
    ASSERT_TRUE(writeFile(broken, synth::randomBytes(50'000, 3)));
    const QString longOne = album(u"03 - Long.flac"_s);
    ASSERT_TRUE(writeFile(longOne, synth::makeFlac(synth::quantised(synth::sine(220.0, 90.0), 16))));
    service->add({broken, longOne});
    QList<ConverterItem> items = settled();
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items.at(0).state, State::Failed);
    EXPECT_FALSE(items.at(0).message.isEmpty());
    ASSERT_EQ(items.at(1).state, State::Ready);

    service->start();
    service->cancel();
    items = settled();
    EXPECT_EQ(items.at(1).state, State::Cancelled);
    EXPECT_FALSE(QFileInfo::exists(album(u"03 - Long.mp3"_s)));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"staging/converter/2/03 - Long.mp3"_s)));

    service->clearFinished();
    EXPECT_TRUE(service->items().isEmpty());
}
