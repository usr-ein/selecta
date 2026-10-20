// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "core/InboxService.h"
#include "core/MetaServices.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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
    SettingsView(const BuildInfo &info, Appearance appearance, core::LibraryConfig library, QWidget *parent = nullptr);

    // Reflects an appearance chosen elsewhere (menu, toolbar) without emitting appearanceRequested.
    void setAppearance(Appearance appearance);
    core::LibraryConfig libraryConfig() const { return m_library; }

    // Changes the library configuration as if the user had (for tests and the debug socket, which can't drive folder
    // pickers). Returns the reason it was refused, or an empty string.
    QString applyLibraryConfig(const core::LibraryConfig &config);

    // Shows the engine's view of the Spotify set-up.
    void setSpotifyStatus(const core::SpotifyStatus &status);

Q_SIGNALS:
    void appearanceRequested(selecta::app::Appearance appearance);
    void libraryConfigChanged(const selecta::core::LibraryConfig &config);
    // Save pressed: an empty secret with keepSavedSecret leaves the saved one in place.
    void spotifyCredentialsSubmitted(const QString &clientId, const QByteArray &secret, bool keepSavedSecret);
    void spotifySignInRequested();
    void spotifySignOutRequested();

private:
    QWidget *createLibraryBox(QWidget *parent);
    QWidget *createFilingBox(QWidget *parent);
    QWidget *createSpotifyBox(QWidget *parent);
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
};

} // namespace selecta::app
