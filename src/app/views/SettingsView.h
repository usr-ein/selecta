// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "core/Engine.h"
#include "core/InboxService.h"
#include "core/MetaServices.h"
#include "core/NetworkService.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

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
    SettingsView(const BuildInfo &info, Appearance appearance, core::LibraryConfig library,
                 core::NetworkConfig networkConfig, QWidget *parent = nullptr);

    // Reflects an appearance chosen elsewhere (menu, toolbar) without emitting appearanceRequested.
    void setAppearance(Appearance appearance);
    core::LibraryConfig libraryConfig() const { return m_library; }

    // Changes the library configuration as if the user had (for tests and the debug socket, which can't drive folder
    // pickers). Returns the reason it was refused, or an empty string.
    QString applyLibraryConfig(const core::LibraryConfig &config);

    // Shows the engine's view of the Spotify set-up.
    void setSpotifyStatus(const core::SpotifyStatus &status);
    // Shows the connection to the Soulseek server.
    void setNetworkStatus(const core::NetworkStatus &status);
    // Previews (PREV-11, PREV-12): where Space previews from, the yt-dlp to run, and what a check of it found.
    void setPreviewSettings(core::PreviewSource source, const QString &ytDlpPath);
    void setYtDlpStatus(bool ok, const QString &description);
    core::NetworkConfig networkConfig() const { return m_network; }
    // Caches (CACHE-2): what each holds.
    void setCacheSizes(const core::CacheStatus &engine, qint64 waveformBytes);
    // "Show notifications" (UI-6), reflected without emitting showNotificationsChanged.
    void setShowNotifications(bool show);

Q_SIGNALS:
    void appearanceRequested(selecta::app::Appearance appearance);
    void libraryConfigChanged(const selecta::core::LibraryConfig &config);
    // Save pressed: an empty secret with keepSavedSecret leaves the saved one in place.
    void spotifyCredentialsSubmitted(const QString &clientId, const QByteArray &secret, bool keepSavedSecret);
    void spotifySignInRequested();
    void spotifySignOutRequested();
    void networkConfigChanged(const selecta::core::NetworkConfig &config);
    void networkPasswordSubmitted(const QString &password);
    void connectRequested();
    void disconnectRequested();
    void previewSourceChanged(selecta::core::PreviewSource source);
    // An empty path: look in the usual places. The view asks for a check right after.
    void ytDlpPathChanged(const QString &path);
    void ytDlpCheckRequested();
    void clearSearchCacheRequested();
    void clearPreviewCacheRequested();
    void clearWaveformCacheRequested();
    void showNotificationsChanged(bool show);

private:
    QWidget *createLibraryBox(QWidget *parent);
    QWidget *createFilingBox(QWidget *parent);
    QWidget *createSpotifyBox(QWidget *parent);
    QWidget *createNetworkBox(QWidget *parent);
    QWidget *createPreviewBox(QWidget *parent);
    QWidget *createCacheBox(QWidget *parent);
    QWidget *createNotificationBox(QWidget *parent);
    void submitNetwork();
    void submitSpotify();
    void showLibrary();
    void refreshPreview();
    void chooseInbox();
    void addCollection();
    void removeCollection();
    void editsFinished();

    QComboBox *m_appearance;
    core::LibraryConfig m_library;
    QLineEdit *m_inbox = nullptr;
    QListWidget *m_collections = nullptr;
    QLineEdit *m_singlesFolder = nullptr;
    QLineEdit *m_albumsFolder = nullptr;
    QLineEdit *m_singleTemplate = nullptr;
    QLineEdit *m_albumFolderTemplate = nullptr;
    QLineEdit *m_albumTrackTemplate = nullptr;
    QCheckBox *m_clean = nullptr;
    QComboBox *m_separator = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_libraryError = nullptr;
    core::SpotifyStatus m_spotify;
    QLabel *m_spotifyState = nullptr;
    QLineEdit *m_spotifyClientId = nullptr;
    QLineEdit *m_spotifySecret = nullptr;
    QPushButton *m_spotifySave = nullptr;
    QPushButton *m_spotifySignIn = nullptr;
    QPushButton *m_spotifySignOut = nullptr;
    core::NetworkConfig m_network;
    core::NetworkStatus m_networkStatus;
    QLabel *m_networkState = nullptr;
    QLineEdit *m_networkHost = nullptr;
    QSpinBox *m_networkPort = nullptr;
    QLineEdit *m_networkUser = nullptr;
    QLineEdit *m_networkPassword = nullptr;
    QSpinBox *m_listenPort = nullptr;
    QCheckBox *m_connectAtLaunch = nullptr;
    QPushButton *m_networkSave = nullptr;
    QPushButton *m_networkConnect = nullptr;
    QComboBox *m_previewSource = nullptr;
    QLineEdit *m_ytDlpPath = nullptr;
    QLabel *m_ytDlpState = nullptr;
    QLabel *m_searchCache = nullptr;
    QLabel *m_previewCache = nullptr;
    QLabel *m_waveformCache = nullptr;
    QCheckBox *m_showNotifications = nullptr;
};

} // namespace selecta::app
