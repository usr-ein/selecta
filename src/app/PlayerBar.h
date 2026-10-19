// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

namespace selecta::app {

// The persistent player at the bottom of the window (PRD 8.2): track info and the always-dark waveform screen
// (UI-3). Until previews exist (milestone M5) it shows that nothing is playing; it has no controls that can't work.
class PlayerBar : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);
};

} // namespace selecta::app
