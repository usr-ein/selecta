// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"

#include <QWidget>

class QComboBox;

namespace selecta::app {

// Facts shown under "About this build".
struct BuildInfo
{
    QString version;
    QString qtVersion;
    QString buildType;
    bool liveServerCompiledIn = false;
    int session = 0;
    QString dataDir;
    QString logDir;
};

// Settings (PRD 8.2). Every row here works; settings arrive together with the features they control.
class SettingsView : public QWidget
{
    Q_OBJECT

public:
    SettingsView(const BuildInfo &info, Appearance appearance, QWidget *parent = nullptr);

    // Reflects an appearance chosen elsewhere (menu, toolbar) without emitting appearanceRequested.
    void setAppearance(Appearance appearance);

Q_SIGNALS:
    void appearanceRequested(selecta::app::Appearance appearance);

private:
    QComboBox *m_appearance;
};

} // namespace selecta::app
