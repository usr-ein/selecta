// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "core/FileRanking.h"
#include "core/InboxService.h"
#include "core/NetworkService.h"
#include "core/SearchFilter.h"

#include <QByteArray>
#include <QSettings>
#include <QString>

namespace selecta::app {

// User preferences and window state, in an INI file inside the data folder (so `--data-dir` isolates them too).
// Main thread only.
class AppSettings
{
public:
    explicit AppSettings(const QString &iniPath);

    Appearance appearance() const;
    void setAppearance(Appearance appearance);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray &geometry);
    QByteArray splitterState(const QString &name) const;
    void setSplitterState(const QString &name, const QByteArray &state);

    bool sidebarShown() const;
    void setSidebarShown(bool shown);
    bool inspectorShown() const;
    void setInspectorShown(bool shown);

    QString lastView() const;
    void setLastView(const QString &id);

    // "Pause network activity" (SAFE-5) stays on across restarts until the user resumes.
    bool networkPaused() const;
    void setNetworkPaused(bool paused);

    // The player bar's volume, 0…1.
    double previewVolume() const;
    void setPreviewVolume(double volume);

    // Settings → Library and Filing (PRD LIB-1, DL-5, DL-13). Missing or invalid values read as the defaults.
    core::LibraryConfig libraryConfig() const;
    void setLibraryConfig(const core::LibraryConfig &config);

    // Settings → Soulseek. The password isn't here: it lives in the Keychain.
    core::NetworkConfig networkConfig() const;
    void setNetworkConfig(const core::NetworkConfig &config);

    // The Search view: the quality preset and the filters (PRD SRCH-8), the folder grouping (SRCH-10), and whether the
    // filters are shown. `customised`: the filters were changed from the preset's.
    struct SearchViewState
    {
        core::QualityWanted preset = core::QualityWanted::Mp3_320OrLossless;
        core::SearchFilter filter = core::presetFilter(core::QualityWanted::Mp3_320OrLossless);
        bool customised = false;
        bool grouped = false;
        bool filtersShown = false;
    };
    SearchViewState searchViewState() const;
    void setSearchViewState(const SearchViewState &state);

    // Writes pending changes to disk. Returns false if the file can't be written.
    bool sync();
    QString fileName() const { return m_settings.fileName(); }

private:
    QSettings m_settings;
};

} // namespace selecta::app
