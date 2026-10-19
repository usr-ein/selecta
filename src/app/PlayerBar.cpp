// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/PlayerBar.h"

#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kScreenHeight = 120;

} // namespace

PlayerBar::PlayerBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(u"player"_s);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);

    auto *title = new QLabel(tr("Nothing playing"), this);
    title->setObjectName(u"player.title"_s);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);

    auto *subtitle = new QLabel(tr("Previews and their waveform appear here."), this);
    subtitle->setObjectName(u"player.subtitle"_s);
    subtitle->setForegroundRole(QPalette::PlaceholderText);

    // The waveform screen stays dark in both appearances, like a CDJ (UI-3).
    auto *screen = new QFrame(this);
    screen->setObjectName(u"player.screen"_s);
    screen->setFixedHeight(kScreenHeight);
    screen->setStyleSheet(u"#player\\.screen { background: #0b0b0d; border-radius: 8px; }"_s);
    auto *screenText = new QLabel(tr("No preview"), screen);
    screenText->setObjectName(u"player.screenText"_s);
    screenText->setAlignment(Qt::AlignCenter);
    screenText->setStyleSheet(u"color: rgba(255, 255, 255, 0.35);"_s);
    auto *screenLayout = new QVBoxLayout(screen);
    screenLayout->addWidget(screenText);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 12);
    layout->setSpacing(2);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addSpacing(8);
    layout->addWidget(screen);
}

} // namespace selecta::app
