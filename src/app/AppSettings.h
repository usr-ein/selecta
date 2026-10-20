// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "core/Engine.h"
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
    // Where Space previews from (PREV-12, D26); ⇧Space always asks the peer for its file.
    core::PreviewSource previewSource() const;
    void setPreviewSource(core::PreviewSource source);
    // Previews follow the selection as it moves with ↑ and ↓ (PREV-4).
    bool previewFollowsSelection() const;
    void setPreviewFollowsSelection(bool follows);
    // Settings → Previews: the yt-dlp to run; empty: look in the usual places.
    QString ytDlpPath() const;
    void setYtDlpPath(const QString &path);

    // Settings → "Show notifications" (UI-6): system notifications while Selecta is in the background. On by default.
    bool showNotifications() const;
    void setShowNotifications(bool show);

    // The user accepted the one-time notice on Spotify's terms (PRD META-4, D30); until then Spotify isn't used.
    bool spotifyTermsAccepted() const;
    void setSpotifyTermsAccepted(bool accepted);

    // SRCH-15: the searches typed lately, newest first, at most kRecentSearches.
    static constexpr int kRecentSearches = 30;
    QStringList recentSearches() const;
    void addRecentSearch(const QString &text);

    // The Converter (PRD CONV-1, CONV-3): the format chosen ("mp3-320", "mp3-256", "mp3-192", "mp3-v0", "mp3-v2",
    // "aiff", "wav", "flac"), ID3v2.4 rather than 2.3, the cover's longest side (0: as it is), and the folder new files
    // go to (empty: next to each original).
    struct ConverterState
    {
        QString target = QStringLiteral("mp3-320");
        bool id3v24 = false;
        int artworkMax = 800;
        QString outputFolder;
    };
    ConverterState converterState() const;
    void setConverterState(const ConverterState &state);

    // Settings → Library and Filing (PRD LIB-1, DL-5, DL-13). Missing or invalid values read as the defaults.
    core::LibraryConfig libraryConfig() const;
    void setLibraryConfig(const core::LibraryConfig &config);

    // Settings → Soulseek. The password isn't here: it lives in the Keychain.
    core::NetworkConfig networkConfig() const;
    void setNetworkConfig(const core::NetworkConfig &config);

    // The Search view: the filter preset and the filters (PRD SRCH-8), the folder grouping (SRCH-10), and whether the
    // filters are shown. `customised`: the filters were changed from the preset's.
    struct SearchViewState
    {
        QString presetId = core::kMp3OrLosslessPreset.toString();
        core::SearchFilter filter = core::builtInPresets().constFirst().filter;
        bool customised = false;
        bool grouped = false;
        bool filtersShown = false;
    };
    SearchViewState searchViewState() const;
    void setSearchViewState(const SearchViewState &state);

    // The filter presets (PRD SRCH-8): the user's own (Selecta's are core::builtInPresets()), the default one, and the
    // one Get tracks uses. Invalid entries are skipped; an unknown id reads as Selecta's default.
    QList<core::FilterPreset> userPresets() const;
    void setUserPresets(const QList<core::FilterPreset> &presets);
    QString defaultPresetId() const;
    void setDefaultPresetId(const QString &id);
    QString getTracksPresetId() const;
    void setGetTracksPresetId(const QString &id);
    // Selecta's presets, then the user's.
    QList<core::FilterPreset> presets() const;
    std::optional<core::FilterPreset> presetById(const QString &id) const;

    // Writes pending changes to disk. Returns false if the file can't be written.
    bool sync();
    QString fileName() const { return m_settings.fileName(); }

private:
    QSettings m_settings;
};

} // namespace selecta::app
