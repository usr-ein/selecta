// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "core/InboxService.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

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

Q_SIGNALS:
    void appearanceRequested(selecta::app::Appearance appearance);
    void libraryConfigChanged(const selecta::core::LibraryConfig &config);

private:
    QWidget *createLibraryBox(QWidget *parent);
    QWidget *createFilingBox(QWidget *parent);
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
};

} // namespace selecta::app
