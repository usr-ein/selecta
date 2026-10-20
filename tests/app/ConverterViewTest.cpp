// SPDX-License-Identifier: GPL-3.0-or-later
// PRD CONV-1: the Converter's formats, sample rates and bit depths, as the bar offers them and the rows advise them.
#include "app/views/ConverterView.h"

#include "SyntheticAudio.h"
#include "app/AppSettings.h"
#include "app/models/ConverterModel.h"
#include "app/views/ConverterInspector.h"
#include "core/EngineHost.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::AppSettings;
using selecta::app::ConverterInspector;
using selecta::app::ConverterModel;
using selecta::app::ConverterView;
using selecta::convert::OutputFormat;
using selecta::core::ConverterItem;
using State = ConverterItem::State;
namespace synth = selecta::testing;

namespace {

class ConverterViewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        settings = std::make_unique<AppSettings>(dir.filePath(u"settings.ini"_s));
    }

    void TearDown() override
    {
        view.reset();
        if (engine) {
            engine->stop();
        }
    }

    // The engine converts; without it the bar still works, and the rows stay empty.
    void startEngine()
    {
        selecta::core::EngineOptions options;
        options.databasePath = dir.filePath(u"engine.sqlite3"_s);
        options.stagingDir = dir.filePath(u"staging"_s);
        options.metaEndpoints = selecta::core::MetaEndpoints::unreachable(); // tests never reach real services
        options.watchCollection = false;
        engine = std::make_unique<selecta::core::EngineHost>(options);
        QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
        engine->start();
        ASSERT_TRUE(started.wait(5000));
        ASSERT_TRUE(started.constFirst().at(0).toBool());
    }

    void makeView()
    {
        if (!engine) {
            engine = std::make_unique<selecta::core::EngineHost>(selecta::core::EngineOptions{});
        }
        view = std::make_unique<ConverterView>(*engine, *settings);
        view->setAttribute(Qt::WA_DontShowOnScreen);
        view->show();
    }

    template <typename Widget>
    Widget *child(const QString &name) const
    {
        auto *widget = view->findChild<Widget *>(name);
        EXPECT_NE(widget, nullptr) << name.toStdString();
        return widget;
    }

    QComboBox *target() const { return child<QComboBox>(u"converter.target"_s); }
    QComboBox *sampleRate() const { return child<QComboBox>(u"converter.sampleRate"_s); }
    QComboBox *bitDepth() const { return child<QComboBox>(u"converter.bitDepth"_s); }
    QComboBox *id3() const { return child<QComboBox>(u"converter.id3"_s); }

    static void choose(QComboBox *box, const QVariant &data)
    {
        const int index = box->findData(data);
        ASSERT_GE(index, 0) << data.toString().toStdString();
        box->setCurrentIndex(index);
    }

    QString write(const QString &name, const QByteArray &content) const
    {
        const QString path = dir.filePath(u"Okuma Drift - Night Signals (2024)/"_s + name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly) && file.write(content) == content.size());
        return path;
    }

    // Waits until the only row satisfies `done`, and returns it.
    ConverterItem waitForRow(const std::function<bool(const ConverterItem &)> &done) const
    {
        ConverterModel *model = view->model();
        EXPECT_TRUE(
            QTest::qWaitFor([&] { return model->rowCount() == 1 && done(model->items().constFirst()); }, 30'000))
            << (model->rowCount() == 1 ? ConverterModel::resultText(model->items().constFirst()).toStdString()
                                       : std::string("no row"));
        return model->rowCount() == 1 ? model->items().constFirst() : ConverterItem();
    }

    static bool ready(const ConverterItem &item) { return item.state == State::Ready; }

    QTemporaryDir dir;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<selecta::core::EngineHost> engine;
    std::unique_ptr<ConverterView> view;
};

} // namespace

TEST_F(ConverterViewTest, EachFormatTakesItsOwnRatesAndLosslessOnesABitDepth)
{
    makeView();
    ASSERT_GE(target()->findData(u"alac"_s), 0);
    EXPECT_EQ(target()->itemText(target()->findData(u"alac"_s)), u"ALAC (Apple Lossless)"_s);
    const auto items = [](const QComboBox *box) {
        QStringList texts;
        for (int index = 0; index < box->count(); ++index) {
            texts.append(box->itemText(index));
        }
        return texts;
    };
    EXPECT_EQ(sampleRate()->accessibleName(), u"Sample rate"_s);
    EXPECT_EQ(bitDepth()->accessibleName(), u"Bit depth"_s);
    EXPECT_EQ(items(bitDepth()), (QStringList{u"Automatic bit depth"_s, u"16-bit"_s, u"24-bit"_s}));

    // MP3, the default: the rates MP3 stores, and no bit depth.
    EXPECT_EQ(target()->currentData().toString(), u"mp3-320"_s);
    EXPECT_EQ(items(sampleRate()), (QStringList{u"Automatic rate"_s, u"32 kHz"_s, u"44.1 kHz"_s, u"48 kHz"_s}));
    EXPECT_TRUE(sampleRate()->isEnabled());
    EXPECT_FALSE(bitDepth()->isEnabled());
    EXPECT_EQ(bitDepth()->currentIndex(), 0);
    choose(sampleRate(), 48000);
    EXPECT_EQ(view->options().target.sampleRate, 48000);
    EXPECT_EQ(view->options().target.bitDepth, 0);
    EXPECT_EQ(settings->converterState().mp3SampleRate, 48000);

    // AIFF 24-bit / 96 kHz: what the converter gets, and what is kept.
    choose(target(), u"aiff"_s);
    EXPECT_EQ(items(sampleRate()), (QStringList{u"Automatic rate"_s, u"44.1 kHz"_s, u"48 kHz"_s, u"88.2 kHz"_s,
                                                u"96 kHz"_s, u"176.4 kHz"_s, u"192 kHz"_s}));
    EXPECT_EQ(sampleRate()->currentText(), u"Automatic rate"_s); // MP3's choice is MP3's
    EXPECT_TRUE(bitDepth()->isEnabled());
    choose(sampleRate(), 96000);
    choose(bitDepth(), 24);
    selecta::core::ConverterOptions options = view->options();
    EXPECT_EQ(options.target.format, OutputFormat::Aiff);
    EXPECT_EQ(options.target.sampleRate, 96000);
    EXPECT_EQ(options.target.bitDepth, 24);
    const AppSettings::ConverterState state = settings->converterState();
    EXPECT_EQ(state.target, u"aiff"_s);
    EXPECT_EQ(state.sampleRate, 96000);
    EXPECT_EQ(state.bitDepth, 24);
    EXPECT_EQ(state.mp3SampleRate, 48000);

    // MP3 again: its own rate, no bit depth; the lossless choices come back with a lossless format.
    choose(target(), u"mp3-320"_s);
    EXPECT_EQ(sampleRate()->currentText(), u"48 kHz"_s);
    EXPECT_EQ(bitDepth()->currentText(), u"Automatic bit depth"_s);
    options = view->options();
    EXPECT_EQ(options.target.format, OutputFormat::Mp3);
    EXPECT_EQ(options.target.sampleRate, 48000);
    EXPECT_EQ(options.target.bitDepth, 0);
    choose(target(), u"flac"_s);
    EXPECT_EQ(sampleRate()->currentText(), u"96 kHz"_s);
    EXPECT_EQ(bitDepth()->currentText(), u"24-bit"_s);
    EXPECT_EQ(view->options().target.format, OutputFormat::Flac);
    EXPECT_EQ(view->options().target.sampleRate, 96000);

    // And so does the next window, from the settings on disk.
    ASSERT_TRUE(settings->sync());
    view.reset();
    settings = std::make_unique<AppSettings>(dir.filePath(u"settings.ini"_s));
    makeView();
    EXPECT_EQ(target()->currentData().toString(), u"flac"_s);
    EXPECT_EQ(view->options().target.sampleRate, 96000);
    EXPECT_EQ(view->options().target.bitDepth, 24);
    choose(target(), u"mp3-v0"_s);
    EXPECT_EQ(view->options().target.sampleRate, 48000);
}

TEST_F(ConverterViewTest, SettingsOnlyKeepRatesAndBitDepthsTheBarOffers)
{
    {
        QSettings ini(dir.filePath(u"settings.ini"_s), QSettings::IniFormat);
        ini.setValue(u"converter/target"_s, u"alac"_s);
        ini.setValue(u"converter/sampleRate"_s, 12345);
        ini.setValue(u"converter/bitDepth"_s, 20);
        ini.setValue(u"converter/mp3SampleRate"_s, 96000); // not an MP3 rate
    }
    settings = std::make_unique<AppSettings>(dir.filePath(u"settings.ini"_s));
    AppSettings::ConverterState state = settings->converterState();
    EXPECT_EQ(state.target, u"alac"_s);
    EXPECT_EQ(state.sampleRate, 0);
    EXPECT_EQ(state.bitDepth, 0);
    EXPECT_EQ(state.mp3SampleRate, 0);

    state.sampleRate = 176400;
    state.bitDepth = 16;
    settings->setConverterState(state);
    EXPECT_EQ(settings->converterState().sampleRate, 176400);
    EXPECT_EQ(settings->converterState().bitDepth, 16);
}

TEST_F(ConverterViewTest, Id3IsOnlyOfferedForFormatsThatUseIt)
{
    makeView();
    for (const QString &id : {u"mp3-320"_s, u"mp3-v0"_s, u"aiff"_s, u"wav"_s}) {
        choose(target(), id);
        EXPECT_TRUE(id3()->isEnabled()) << id.toStdString();
    }
    choose(target(), u"flac"_s);
    EXPECT_FALSE(id3()->isEnabled());
    EXPECT_EQ(id3()->toolTip(), u"FLAC keeps its tags as Vorbis comments."_s);
    choose(target(), u"alac"_s);
    EXPECT_FALSE(id3()->isEnabled());
    EXPECT_EQ(id3()->toolTip(), u"ALAC keeps its tags as MP4 atoms."_s);
    EXPECT_EQ(view->options().target.format, OutputFormat::Alac);

    // A window opening on ALAC starts with it off too.
    view.reset();
    makeView();
    EXPECT_EQ(target()->currentData().toString(), u"alac"_s);
    EXPECT_FALSE(id3()->isEnabled());
}

TEST_F(ConverterViewTest, TheInspectorShowsAlacTagsAsMp4Atoms)
{
    selecta::core::ConverterOptions alac;
    alac.target.format = OutputFormat::Alac;
    EXPECT_EQ(ConverterInspector::writtenAs(u"TITLE"_s, alac), u"©nam"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"ARTIST"_s, alac), u"©ART"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"ALBUMARTIST"_s, alac), u"aART"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"TRACKNUMBER"_s, alac), u"trkn"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"DATE"_s, alac), u"©day"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"BPM"_s, alac), u"tmpo"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"INITIALKEY"_s, alac), u"----:com.apple.iTunes:KEY"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"LABEL"_s, alac), u"----:com.apple.iTunes:LABEL"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"ENCODEDBY"_s, alac), u"©enc"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"MOOD"_s, alac), u"----:com.apple.iTunes:MOOD"_s);
    // The other formats as before.
    selecta::core::ConverterOptions other;
    EXPECT_EQ(ConverterInspector::writtenAs(u"DATE"_s, other), u"TYER"_s);
    EXPECT_EQ(ConverterInspector::writtenAs(u"MOOD"_s, other), u"TXXX:MOOD"_s);
    other.id3 = selecta::convert::Id3Version::V24;
    EXPECT_EQ(ConverterInspector::writtenAs(u"DATE"_s, other), u"TDRC"_s);
    other.target.format = OutputFormat::Flac;
    EXPECT_EQ(ConverterInspector::writtenAs(u"DATE"_s, other), u"DATE"_s);

    ConverterInspector inspector;
    ConverterItem item;
    item.id = 1;
    item.path = dir.filePath(u"01 - Neon Rain.flac"_s);
    item.state = State::Ready;
    item.quality = u"FLAC 16-bit / 44.1 kHz"_s;
    item.tags.set(u"TITLE"_s, u"Neon Rain"_s, selecta::convert::Provenance::Embedded);
    item.tags.set(u"INITIALKEY"_s, u"8A"_s, selecta::convert::Provenance::Embedded);
    inspector.showItem(item, alac);
    auto *subtitle = inspector.findChild<QLabel *>(u"inspector.converter.subtitle"_s);
    auto *tags = inspector.findChild<QLabel *>(u"inspector.converter.tags"_s);
    auto *explain = inspector.findChild<QLabel *>(u"inspector.converter.explain"_s);
    ASSERT_NE(subtitle, nullptr);
    ASSERT_NE(tags, nullptr);
    ASSERT_NE(explain, nullptr);
    EXPECT_EQ(subtitle->text(), u"FLAC 16-bit / 44.1 kHz → ALAC · MP4 atoms"_s);
    EXPECT_TRUE(tags->text().contains(u"Title · ©nam"_s)) << tags->text().toStdString();
    EXPECT_TRUE(tags->text().contains(u"Key · ----:com.apple.iTunes:KEY"_s)) << tags->text().toStdString();
    EXPECT_TRUE(explain->text().startsWith(u"ALAC files (.m4a) keep their tags as MP4 atoms"_s))
        << explain->text().toStdString();
}

TEST_F(ConverterViewTest, EachRowSaysWhatTheChosenRateAndBitDepthGive)
{
    ASSERT_NO_FATAL_FAILURE(startEngine());
    makeView();
    // Noise up to 48 kHz on all 24 bits: a genuine hi-res file.
    const QString source = write(u"03 - Glass Choir.flac"_s,
                                 synth::makeFlac(synth::quantised(synth::whiteNoise(1.0, 7, 96000), 24), {.bits = 24}));
    view->addPaths({source});
    ConverterItem item = waitForRow(ready);
    EXPECT_EQ(item.quality, u"FLAC 24-bit / 96 kHz"_s);

    // AIFF 16-bit / 44.1 kHz: resampled and dithered, and convertible at once.
    choose(target(), u"aiff"_s);
    choose(sampleRate(), 44100);
    choose(bitDepth(), 16);
    item = waitForRow([](const ConverterItem &row) { return row.verdict.startsWith(u"AIFF, 16-bit, 44.1 kHz"_s); });
    EXPECT_FALSE(item.askFirst);
    EXPECT_TRUE(item.notices.join(u'\n').contains(u"Resampled to 44.1 kHz"_s));
    EXPECT_TRUE(item.notices.join(u'\n').contains(u"Reduced to 16-bit with dither"_s));
    EXPECT_EQ(view->summary(), u"1 file → AIFF, 16-bit, 44.1 kHz"_s);
    auto *convert = child<QPushButton>(u"converter.convert"_s);
    ASSERT_NE(convert, nullptr);
    EXPECT_TRUE(convert->isEnabled());

    // 192 kHz: an upsample, which waits for "Convert anyway".
    choose(sampleRate(), 192000);
    item = waitForRow([](const ConverterItem &row) { return row.askFirst; });
    EXPECT_TRUE(item.notices.join(u'\n').contains(u"Raising 96 kHz to 192 kHz adds no information"_s));
    ConverterModel *model = view->model();
    const QModelIndex anyway = model->index(0, ConverterModel::AnywayColumn);
    EXPECT_TRUE(model->flags(anyway).testFlag(Qt::ItemIsUserCheckable));
    EXPECT_EQ(model->data(model->index(0, ConverterModel::ResultColumn)).toString(),
              u"No quality gain — larger file"_s);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !convert->isEnabled(); }, 5000));
    ASSERT_TRUE(model->setData(anyway, Qt::Checked, Qt::CheckStateRole));
    ASSERT_TRUE(QTest::qWaitFor([&] { return convert->isEnabled(); }, 5000));

    // Back to MP3: the rate follows the source again, as far as MP3 can (D-4).
    choose(target(), u"mp3-320"_s);
    item = waitForRow([](const ConverterItem &row) { return row.verdict.startsWith(u"MP3 320 kbps CBR"_s); });
    EXPECT_FALSE(item.askFirst);
    EXPECT_TRUE(item.notices.join(u'\n').contains(u"Resampled to 44.1 kHz"_s));
    EXPECT_EQ(view->summary(), u"1 file → MP3 320 kbps CBR"_s);
}

TEST_F(ConverterViewTest, AnMp3At320IsCopiedAndNeverMadeLossless24Bit)
{
    ASSERT_NO_FATAL_FAILURE(startEngine());
    makeView();
    const QByteArray original = synth::makeMp3(synth::whiteNoise(1.0, 5), {.bitrateKbps = 320});
    const QString source = write(u"02 - Low Tide.mp3"_s, original);
    view->addPaths({source});
    ConverterItem item = waitForRow(ready);
    ConverterModel *model = view->model();
    EXPECT_EQ(model->data(model->index(0, ConverterModel::ResultColumn)).toString(),
              u"Same format · audio copied as is"_s);

    // AIFF 24-bit isn't offered at all; 16-bit asks first.
    choose(target(), u"aiff"_s);
    choose(bitDepth(), 24);
    item = waitForRow([](const ConverterItem &row) { return row.refused; });
    EXPECT_EQ(model->data(model->index(0, ConverterModel::ResultColumn)).toString(),
              u"Not offered: a lossy file can't fill a higher sample rate or bit depth"_s);
    EXPECT_FALSE(model->flags(model->index(0, ConverterModel::AnywayColumn)).testFlag(Qt::ItemIsUserCheckable));
    auto *convert = child<QPushButton>(u"converter.convert"_s);
    ASSERT_NE(convert, nullptr);
    EXPECT_FALSE(convert->isEnabled());
    choose(bitDepth(), 16);
    item = waitForRow([](const ConverterItem &row) { return row.askFirst; });
    EXPECT_FALSE(item.refused);

    // MP3 320: copied as it is, next to the original.
    choose(target(), u"mp3-320"_s);
    waitForRow([](const ConverterItem &row) { return row.verdict == u"Same format · audio copied as is"_s; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return convert->isEnabled(); }, 5000));
    convert->click();
    item = waitForRow([](const ConverterItem &row) { return row.state == State::Done; });
    EXPECT_TRUE(item.outputPath.endsWith(u"/02 - Low Tide (2).mp3"_s)) << item.outputPath.toStdString();
    QFile copy(item.outputPath);
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_TRUE(copy.readAll().endsWith(original));
}
