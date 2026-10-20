// SPDX-License-Identifier: GPL-3.0-or-later
// PREV-9: the spectrogram view. The colour scale, the grid drawn with 0 Hz at the bottom, the cut-off line, the hover
// text, and what it says without a grid.
#include "app/SpectrogramView.h"

#include <QImage>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::SpectrogramView;
namespace audio = selecta::audio;

namespace {

constexpr QRgb kBackground = qRgb(0x0b, 0x0c, 0x0f);

// Relative luminance (sRGB weights, no linearisation: enough to order colours).
double luminance(QRgb colour)
{
    return (0.2126 * qRed(colour)) + (0.7152 * qGreen(colour)) + (0.0722 * qBlue(colour));
}

// `columns` columns, loud (level `loud`) below `cutoffHz` and silent above it, over 10 s at 44.1 kHz.
audio::Spectrogram lowPassed(double cutoffHz, int columns = 200, int bins = 64, quint8 loud = 200)
{
    QByteArray levels(qsizetype{columns} * bins, '\0');
    for (int c = 0; c < columns; ++c) {
        for (int b = 0; b < bins; ++b) {
            const double low = b * 22050.0 / bins;
            levels[(qsizetype{c} * bins) + b] = static_cast<char>(low < cutoffHz ? loud : 0);
        }
    }
    const audio::Result<audio::Spectrogram> made =
        audio::Spectrogram::fromLevels(44100, qint64{441000}, columns, bins, std::move(levels));
    EXPECT_TRUE(made.ok());
    return made.ok() ? made.value() : audio::Spectrogram();
}

class SpectrogramViewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        view.resize(300, 120);
        view.setAttribute(Qt::WA_DontShowOnScreen);
        view.show();
    }

    QImage shot()
    {
        QImage image = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
        image.setDevicePixelRatio(1.0);
        return image.scaled(view.size()); // logical pixels, whatever the screen's ratio
    }

    // The y of a frequency in the view's logical pixels.
    int yOf(double hz) const { return static_cast<int>(std::lround(view.height() - (hz / 22050.0 * view.height()))); }

    SpectrogramView view;
};

} // namespace

TEST(SpectrogramScale, RunsFromBlackToPaleYellowGettingLighter)
{
    EXPECT_EQ(SpectrogramView::colorForLevel(0), qRgb(0x00, 0x00, 0x04));
    EXPECT_EQ(SpectrogramView::colorForLevel(255), qRgb(0xfc, 0xff, 0xa4));
    // Inferno at 0.5 sits between its 0.4 and 0.6 stops.
    EXPECT_EQ(SpectrogramView::colorForLevel(128), qRgb(0xbd, 0x38, 0x53));
    // Lighter with every step (the order survives greyscale and colour-blind eyes), within rounding.
    double previous = -1.0;
    for (int level = 0; level <= 255; ++level) {
        const double now = luminance(SpectrogramView::colorForLevel(static_cast<quint8>(level)));
        EXPECT_GE(now, previous - 0.5) << level;
        previous = now;
    }
}

TEST_F(SpectrogramViewTest, WithoutAGridItSaysWhy)
{
    view.setMessage(u"Loading…"_s);
    EXPECT_EQ(view.accessibleDescription(), u"Loading…"_s);
    EXPECT_TRUE(view.textAt(QPoint(150, 60)).isEmpty());
    const QImage image = shot();
    EXPECT_EQ(image.pixel(2, 2) | 0xff000000, kBackground | 0xff000000);
    EXPECT_EQ(view.accessibleName(), u"Spectrogram"_s);
}

TEST_F(SpectrogramViewTest, DrawsLowFrequenciesAtTheBottomAndMarksTheCutoff)
{
    view.setSpectrogram(lowPassed(16000.0));
    QImage image = shot();
    // Loud below 16 kHz, silent above: bright at the bottom, black at the top.
    const QRgb loud = SpectrogramView::colorForLevel(200);
    const QRgb low = image.pixel(150, yOf(4000.0)) | 0xff000000;
    EXPECT_NEAR(qRed(low), qRed(loud), 12);
    EXPECT_NEAR(qGreen(low), qGreen(loud), 12);
    EXPECT_LT(luminance(image.pixel(150, yOf(18000.0))), 20.0); // between the 15 and 20 kHz ticks

    // The cut-off: a light dashed line across, its frequency written beside it.
    view.setCutoff(16742);
    image = shot();
    const int y = yOf(16742.0);
    int light = 0;
    for (int x = 60; x < 180; ++x) { // clear of the tick labels on the left and its own on the right
        if (luminance(image.pixel(x, y)) > 150.0 || luminance(image.pixel(x, y - 1)) > 150.0) {
            ++light;
        }
    }
    EXPECT_GT(light, 40) << "a dashed line: most of its pixels lit";
    EXPECT_LT(light, 120) << "dashed, not solid";
    EXPECT_EQ(view.cutoff(), 16742);
    EXPECT_TRUE(view.accessibleDescription().contains(u"stops at 16.7 kHz"_s))
        << view.accessibleDescription().toStdString();

    // No line for a cut-off outside the grid.
    view.setCutoff(30000);
    image = shot();
    EXPECT_LT(luminance(image.pixel(100, yOf(18000.0))), 20.0);
}

TEST_F(SpectrogramViewTest, HoverSaysTimeAndFrequency)
{
    view.setSpectrogram(lowPassed(16000.0));
    // Half way across 10 s, half way up 22.05 kHz.
    EXPECT_EQ(view.textAt(QPoint(150, 60)), u"0:05 · 11 kHz"_s);
    EXPECT_EQ(view.textAt(QPoint(0, 119)), u"0:00 · 0.2 kHz"_s);
    EXPECT_TRUE(view.textAt(QPoint(-1, 60)).isEmpty());
    EXPECT_TRUE(view.textAt(QPoint(150, 120)).isEmpty());
}

TEST_F(SpectrogramViewTest, ClearForgetsEverything)
{
    view.setSpectrogram(lowPassed(16000.0));
    view.setCutoff(16000);
    view.clear();
    EXPECT_TRUE(view.spectrogram().isNull());
    EXPECT_FALSE(view.cutoff().has_value());
    EXPECT_TRUE(view.message().isEmpty());
    EXPECT_TRUE(view.textAt(QPoint(150, 60)).isEmpty());
}
