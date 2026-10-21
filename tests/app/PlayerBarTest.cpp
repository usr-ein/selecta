// SPDX-License-Identifier: GPL-3.0-or-later
// The player bar's transport as macOS players have it (beta notes, 2026-10-20): symbols with no box around them, in
// the middle of the bar.
#include "app/PlayerBar.h"

#include "app/Preview.h"

#include <QAction>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleFactory>
#include <QTest>
#include <QToolButton>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::PlayerBar;
using selecta::app::PreviewController;

namespace {

class PlayerBarTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bar.setAttribute(Qt::WA_DontShowOnScreen);
        bar.show();
    }

    // Where the transport's middle is, across the bar.
    double transportCentre() const
    {
        const auto *back = bar.findChild<QToolButton *>(u"player.back"_s);
        const auto *forward = bar.findChild<QToolButton *>(u"player.forward"_s);
        const int left = back->mapTo(&bar, QPoint(0, 0)).x();
        const int right = forward->mapTo(&bar, QPoint(forward->width(), 0)).x();
        return (left + right) / 2.0;
    }

    PreviewController controller{selecta::playback::AudioOutput::createSilent(), nullptr}; // never the audio device
    PlayerBar bar{controller};
};

// A solid square to stand in for an SF Symbol (the test platform has none).
QIcon squareIcon()
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::black);
    return {pixmap};
}

QImage render(QWidget &widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawChildren); // the button alone, without its background
    return image;
}

} // namespace

TEST_F(PlayerBarTest, TheTransportHasNoBoxEvenWhenPressed)
{
    const std::unique_ptr<QStyle> fusion(QStyleFactory::create(u"Fusion"_s));
    ASSERT_NE(fusion, nullptr);
    for (const QString &name : {u"player.back"_s, u"player.play"_s, u"player.forward"_s}) {
        auto *button = bar.findChild<QToolButton *>(name);
        ASSERT_NE(button, nullptr) << name.toStdString();
        button->setIcon(QIcon()); // no symbol: whatever shows is the button's own box
        button->setEnabled(true); // as while a preview plays
        button->setAttribute(Qt::WA_UnderMouse, true);
        button->setStyle(fusion.get()); // a style that draws a tool button's panel when hovered or pressed
        for (const bool down : {false, true}) {
            button->setDown(down);
            const QImage image = render(*button);
            bool painted = false;
            for (int y = 0; y < image.height() && !painted; ++y) {
                for (int x = 0; x < image.width() && !painted; ++x) {
                    painted = qAlpha(image.pixel(x, y)) != 0;
                }
            }
            EXPECT_FALSE(painted) << name.toStdString() << (down ? " pressed" : "");
        }
        button->setDown(false);
        button->setStyle(nullptr);
    }
}

TEST_F(PlayerBarTest, TheSymbolIsDrawnInTheMiddleOfItsButtonInTheTextColour)
{
    auto *play = bar.findChild<QToolButton *>(u"player.play"_s);
    ASSERT_NE(play, nullptr);
    play->setIcon(squareIcon());
    play->setEnabled(true);
    QPalette palette = play->palette();
    palette.setColor(QPalette::WindowText, QColor(0x20, 0x40, 0x60));
    play->setPalette(palette);
    const QImage image = render(*play);
    const QPoint middle(image.width() / 2, image.height() / 2);
    EXPECT_EQ(QColor(image.pixel(middle)).rgb(), QColor(0x20, 0x40, 0x60).rgb());
    EXPECT_EQ(qAlpha(image.pixel(0, 0)), 0); // the padding around it stays clear

    // Pressed, it dims; disabled, more so.
    play->setDown(true);
    const int pressed = qAlpha(render(*play).pixel(middle));
    play->setDown(false);
    play->setEnabled(false);
    const int disabled = qAlpha(render(*play).pixel(middle));
    EXPECT_LT(pressed, 255);
    EXPECT_LT(disabled, pressed);
}

TEST_F(PlayerBarTest, TheTransportStaysInTheMiddleOfTheBar)
{
    auto *discard = new QAction(squareIcon(), u"Discard"_s, &bar);
    bar.addPreviewAction(discard, u"player.discard"_s); // the right side grows
    auto *title = bar.findChild<QLabel *>(u"player.title"_s);
    ASSERT_NE(title, nullptr);
    for (const int width : {900, 1400}) {
        bar.resize(width, 200);
        QCoreApplication::processEvents();
        EXPECT_NEAR(transportCentre(), bar.width() / 2.0, 2.0) << "at " << width;
    }
    // A long title elides rather than pushing the transport aside.
    title->setText(QString(200, u'x'));
    QCoreApplication::processEvents();
    EXPECT_NEAR(transportCentre(), bar.width() / 2.0, 2.0);
    // The right side stays whole.
    const auto *volume = bar.findChild<QWidget *>(u"player.volume"_s);
    ASSERT_NE(volume, nullptr);
    EXPECT_LE(volume->mapTo(&bar, QPoint(volume->width(), 0)).x(), bar.width());
}
