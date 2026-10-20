// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/WaveformView.h"

#include <QAccessible>
#include <QImage>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::WaveformView;
namespace audio = selecta::audio;

namespace {

constexpr QRgb kBackground = qRgb(0x0b, 0x0c, 0x0f);
constexpr QRgb kLow = qRgb(0x00, 0x55, 0xe1);
constexpr QRgb kMid = qRgb(0xff, 0xa6, 0x00);
constexpr QRgb kOverlap = qRgb(0xb3, 0x66, 0x07);

// `seconds` of detail columns with the given band peaks.
audio::Waveform steady(double seconds, quint8 low, quint8 mid, quint8 high, bool complete = true)
{
    const auto count = static_cast<qsizetype>(seconds * audio::kColumnsPerSecond);
    QList<audio::WaveformColumn> columns(count);
    for (audio::WaveformColumn &column : columns) {
        column.peak = {low, mid, high};
        column.rms = {low, mid, high};
    }
    return audio::Waveform::fromDetail(audio::kColumnsPerSecond, count, columns, complete);
}

class WaveformViewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        view.resize(400, 140);
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.show(); // never exposed off screen: grab() renders it anyway
    }

    QImage shot()
    {
        QImage image = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
        image.setDevicePixelRatio(1.0);
        return image.scaled(view.size()); // logical pixels, whatever the screen's ratio
    }

    // Colours found in a column of the image, from top to bottom (consecutive repeats folded).
    static QList<QRgb> columnColours(const QImage &image, int x, int top, int bottom)
    {
        QList<QRgb> colours;
        for (int y = top; y < bottom; ++y) {
            const QRgb pixel = image.pixel(x, y) | 0xff000000;
            if (colours.isEmpty() || colours.constLast() != pixel) {
                colours.append(pixel);
            }
        }
        return colours;
    }

    WaveformView view;
};

} // namespace

TEST_F(WaveformViewTest, EmptyShowsTheMessageOnTheDarkScreen)
{
    view.setMessage(u"Select a track"_s);
    const QImage image = shot();
    EXPECT_EQ(image.pixel(2, 2) | 0xff000000, kBackground);
    EXPECT_EQ(image.pixel(view.width() - 3, view.height() - 3) | 0xff000000, kBackground);
}

TEST_F(WaveformViewTest, DetailIsMirroredWithTheOverlapInBrown)
{
    // Lows louder than mids: blue beyond a brown overlap, symmetric about the centre; no highs.
    view.setWaveform(steady(20.0, 255, 128, 0));
    view.setDuration(20.0);
    view.setPosition(10.0);
    const QImage image = shot();
    const int x = 60; // left of the playhead and its labels' area below the top
    const int detailBottom = view.height() - 30 - 6;
    const QList<QRgb> colours = columnColours(image, x, 20, detailBottom);
    EXPECT_TRUE(colours.contains(kLow));
    EXPECT_TRUE(colours.contains(kOverlap));
    EXPECT_FALSE(colours.contains(kMid)); // mids never exceed the lows here
    const int middle = detailBottom / 2;
    EXPECT_EQ(image.pixel(x, middle) | 0xff000000, kOverlap);
    // Symmetric: the blue reaches as far up as down.
    int up = 0;
    while (middle - up > 0 && (image.pixel(x, middle - up) | 0xff000000) != kBackground) {
        ++up;
    }
    int down = 0;
    while (middle + down < detailBottom && (image.pixel(x, middle + down) | 0xff000000) != kBackground) {
        ++down;
    }
    EXPECT_NEAR(up, down, 2);
}

TEST_F(WaveformViewTest, OverviewStacksFromTheBottom)
{
    view.setWaveform(steady(30.0, 200, 200, 200));
    view.setDuration(30.0);
    view.setPosition(0.0);
    const QImage image = shot();
    const int x = view.width() / 2;
    const QList<QRgb> colours = columnColours(image, x, view.height() - 30, view.height());
    // Background on top, then white highs, amber mids, and blue lows at the bottom.
    ASSERT_GE(colours.size(), 4);
    EXPECT_EQ(colours.constLast(), kLow);
    EXPECT_EQ(colours.at(colours.size() - 2), kMid);
    EXPECT_EQ(colours.at(colours.size() - 3), qRgb(255, 255, 255));
}

TEST_F(WaveformViewTest, NothingIsDrawnWhereTheAnalysisHasntReached)
{
    // 10 s analysed of a 40 s download: the overview's right half stays dark.
    view.setWaveform(steady(10.0, 255, 255, 255, false));
    view.setDuration(40.0);
    view.setBuffered(10.0);
    const QImage image = shot();
    EXPECT_NE(columnColours(image, view.width() / 8, view.height() - 30, view.height()).indexOf(kLow), -1);
    EXPECT_EQ(columnColours(image, view.width() * 3 / 4, view.height() - 20, view.height()).indexOf(kLow), -1);
}

TEST_F(WaveformViewTest, ClickingTheOverviewSeeks)
{
    view.setWaveform(steady(60.0, 100, 100, 100));
    view.setDuration(60.0);
    const QSignalSpy seeks(&view, &WaveformView::seekRequested);
    QTest::mouseClick(&view, Qt::LeftButton, {}, QPoint(view.width() / 4, view.height() - 10));
    ASSERT_EQ(seeks.size(), 1);
    EXPECT_NEAR(seeks.at(0).at(0).toDouble(), 15.0, 0.2);
    EXPECT_NEAR(view.position(), 15.0, 0.2); // shown before the player catches up
}

TEST_F(WaveformViewTest, DraggingTheDetailScrubs)
{
    view.setWaveform(steady(60.0, 100, 100, 100));
    view.setDuration(60.0);
    view.setPosition(30.0);
    const QSignalSpy seeks(&view, &WaveformView::seekRequested);
    const QPoint start(200, 40);
    QTest::mousePress(&view, Qt::LeftButton, {}, start);
    QTest::mouseMove(&view, start + QPoint(-100, 0)); // a quarter of the width to the left: 2 s later
    QTest::mouseRelease(&view, Qt::LeftButton, {}, start + QPoint(-100, 0));
    ASSERT_EQ(seeks.size(), 1);
    EXPECT_NEAR(seeks.at(0).at(0).toDouble(), 30.0 + (WaveformView::kZoomSeconds / 4.0), 0.1);
}

TEST_F(WaveformViewTest, ClicksDoNothingWithoutATrack)
{
    const QSignalSpy seeks(&view, &WaveformView::seekRequested);
    QTest::mouseClick(&view, Qt::LeftButton, {}, QPoint(100, view.height() - 10));
    EXPECT_TRUE(seeks.isEmpty());
}

TEST_F(WaveformViewTest, VoiceOverSeesASliderWithThePositionTheLengthAndTheSource)
{
    view.setDuration(372.0);
    view.setPosition(83.4);
    view.setSource(u"Preview from vinyl_ghost"_s);
    view.setBuffered(186.0);
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(&view);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->role(), QAccessible::Slider);
    EXPECT_EQ(iface->text(QAccessible::Name), u"Waveform"_s);
    EXPECT_EQ(iface->text(QAccessible::Value), u"1:23 of 6:12"_s);
    EXPECT_EQ(iface->text(QAccessible::Description),
              u"Preview from vinyl_ghost. 50 % downloaded. Left and Right arrows move 5 seconds."_s);
    EXPECT_TRUE(iface->state().focusable);

    QAccessibleValueInterface *value = iface->valueInterface();
    ASSERT_NE(value, nullptr);
    EXPECT_DOUBLE_EQ(value->minimumValue().toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(value->maximumValue().toDouble(), 372.0);
    EXPECT_DOUBLE_EQ(value->minimumStepSize().toDouble(), 5.0);
#ifdef Q_OS_MACOS
    EXPECT_EQ(value->currentValue().toString(), u"1:23 of 6:12"_s); // what the macOS bridge reads out
#else
    EXPECT_DOUBLE_EQ(value->currentValue().toDouble(), 83.4);
#endif

    view.setBuffered(-1.0); // all of it
    EXPECT_EQ(iface->text(QAccessible::Description),
              u"Preview from vinyl_ghost. Left and Right arrows move 5 seconds."_s);
}

TEST_F(WaveformViewTest, VoiceOverStepsFiveSecondsAsTheArrowKeysDo)
{
    view.setDuration(372.0);
    view.setPosition(83.0);
    QSignalSpy seeks(&view, &WaveformView::seekRequested);
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(&view);
    ASSERT_NE(iface, nullptr);
    QAccessibleActionInterface *actions = iface->actionInterface();
    ASSERT_NE(actions, nullptr);
    EXPECT_TRUE(actions->actionNames().contains(QAccessibleActionInterface::increaseAction()));
    EXPECT_TRUE(actions->actionNames().contains(QAccessibleActionInterface::decreaseAction()));

    actions->doAction(QAccessibleActionInterface::increaseAction());
    ASSERT_EQ(seeks.size(), 1);
    EXPECT_DOUBLE_EQ(seeks.at(0).at(0).toDouble(), 88.0);
    EXPECT_EQ(iface->text(QAccessible::Value), u"1:28 of 6:12"_s);
    actions->doAction(QAccessibleActionInterface::decreaseAction());
    actions->doAction(QAccessibleActionInterface::decreaseAction());
    ASSERT_EQ(seeks.size(), 3);
    EXPECT_DOUBLE_EQ(seeks.at(2).at(0).toDouble(), 78.0);

    iface->valueInterface()->setCurrentValue(120.0);
    ASSERT_EQ(seeks.size(), 4);
    EXPECT_DOUBLE_EQ(seeks.at(3).at(0).toDouble(), 120.0);
}

TEST_F(WaveformViewTest, ArrowKeysStepWithinTheTrack)
{
    view.setDuration(372.0);
    view.setPosition(370.0);
    QSignalSpy seeks(&view, &WaveformView::seekRequested);
    QTest::keyClick(&view, Qt::Key_Right);
    ASSERT_EQ(seeks.size(), 1);
    EXPECT_DOUBLE_EQ(seeks.at(0).at(0).toDouble(), 372.0); // the end, not past it

    view.setPosition(2.0);
    QTest::keyClick(&view, Qt::Key_Left);
    ASSERT_EQ(seeks.size(), 2);
    EXPECT_DOUBLE_EQ(seeks.at(1).at(0).toDouble(), 0.0);

    QTest::keyClick(&view, Qt::Key_Right, Qt::AltModifier); // the window's 30-second shortcut, not the waveform's
    EXPECT_EQ(seeks.size(), 2);
}

TEST_F(WaveformViewTest, SeeksAreToldToVoiceOverButPlayingIsNot)
{
    static QList<QAccessible::Event> events;
    events.clear();
    QAccessible::installUpdateHandler([](QAccessibleEvent *event) {
        if (event->object() != nullptr && event->object()->objectName() == u"player.waveform"_s) {
            events.append(event->type());
        }
    });
    view.setDuration(372.0);
    events.clear();
    view.setPosition(10.0);
    view.setPosition(11.0);
    EXPECT_TRUE(events.isEmpty());
    view.step(5.0);
    QAccessible::installUpdateHandler(nullptr);
    EXPECT_EQ(events, QList<QAccessible::Event>{QAccessible::ValueChanged});
}

TEST_F(WaveformViewTest, WithoutATrackVoiceOverReadsTheMessageAndCantStep)
{
    view.setMessage(u"Select a track and press Space to preview it"_s);
    QSignalSpy seeks(&view, &WaveformView::seekRequested);
    QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(&view);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->text(QAccessible::Value), u"Select a track and press Space to preview it"_s);
    EXPECT_TRUE(iface->text(QAccessible::Description).isEmpty());
    iface->actionInterface()->doAction(QAccessibleActionInterface::increaseAction());
    QTest::keyClick(&view, Qt::Key_Right);
    EXPECT_TRUE(seeks.isEmpty());

    view.setDuration(200.0);
    view.setSource(u"Your Inbox"_s);
    view.clear(); // the track is gone, and its source with it
    EXPECT_EQ(iface->text(QAccessible::Value), u"Select a track and press Space to preview it"_s);
}
