// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "app/views/SettingsView.h"

#include <QHash>
#include <QList>
#include <QMainWindow>

#include <functional>
#include <optional>

class QAction;
class QActionGroup;
class QLabel;
class QSplitter;
class QStackedWidget;

namespace selecta::core {
class EngineHost;
}
namespace selecta::storage {
class Database;
class SessionLog;
} // namespace selecta::storage

namespace selecta::app {

class ActivityView;
class AppSettings;
class GetTracksView;
class InboxView;
class Inspector;
class NavModel;
class PlayerBar;
class PreviewController;
class Sidebar;

// What the main window needs from the rest of the app. The referenced objects outlive the window.
struct AppServices
{
    // cppcheck-suppress uninitMemberVarNoCtor ; references are bound by aggregate initialisation
    storage::Database &database; // the main thread's connection
    // cppcheck-suppress uninitMemberVarNoCtor
    storage::SessionLog &sessions;
    // cppcheck-suppress uninitMemberVarNoCtor
    AppSettings &settings;
    // cppcheck-suppress uninitMemberVarNoCtor
    core::EngineHost &engine;
    // cppcheck-suppress uninitMemberVarNoCtor
    PreviewController &preview;
    BuildInfo build;
    qint64 sessionStartedAtMs = 0;
};

// The single main window (PRD 8.2): toolbar, sidebar, content views, inspector and player. Main thread only.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(AppServices services, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Shows the view with this id (NavModel ids). Returns false, changing nothing, for an unknown id.
    bool navigateTo(const QString &id);
    QString currentViewId() const { return m_currentId; }
    QStringList viewIds() const;
    QString currentTitle() const;
    QString currentSubtitle() const;

    bool isSidebarShown() const;
    void setSidebarShown(bool shown);
    bool isInspectorShown() const;
    void setInspectorShown(bool shown);

    Appearance appearance() const { return m_appearance; }
    void setAppearance(Appearance appearance);

    // Writes window state and preferences to the settings file. Called on close and on quit; safe to call twice.
    void saveState();

    // Space: previews what is selected in the current view, or plays and pauses what is loaded.
    void playOrPause();
    PreviewController &preview() { return m_services.preview; }

Q_SIGNALS:
    void viewChanged(const QString &id);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct View
    {
        QString id;
        std::function<QString()> subtitle;
        QWidget *widget = nullptr;
    };

    void createActions();
    void createMenus();
    void createToolBar();
    void createCentralArea();
    void createViews();
    void addView(const QString &id, QWidget *widget, std::function<QString()> subtitle);
    void restoreSavedState();
    void refreshTitle();
    void refreshNetworkState();
    void previewInboxSelection(bool toggleIfLoaded);
    void previewGetTracksLine(bool toggleIfLoaded);

    AppServices m_services;
    Appearance m_appearance;
    NavModel *m_navModel = nullptr;
    Sidebar *m_sidebar = nullptr;
    QStackedWidget *m_stack = nullptr;
    Inspector *m_inspector = nullptr;
    PlayerBar *m_player = nullptr;
    SettingsView *m_settingsView = nullptr;
    QSplitter *m_mainSplitter = nullptr;
    QSplitter *m_contentSplitter = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QAction *m_toggleSidebar = nullptr;
    QAction *m_toggleInspector = nullptr;
    QAction *m_togglePause = nullptr;
    QAction *m_budget = nullptr;
    ActivityView *m_activityView = nullptr;
    InboxView *m_inboxView = nullptr;
    GetTracksView *m_getTracksView = nullptr;
    QActionGroup *m_appearanceGroup = nullptr;
    QList<QAction *> m_goActions;
    QList<QAction *> m_playbackActions;
    std::optional<bool> m_shownPaused; // what the pause action shows
    qint64 m_askedPreviewLine = 0;     // the Get tracks line a preview was asked for
    qint64 m_playingLine = 0;          // the Get tracks line whose preview is loaded
    qint64 m_previewId = 0;            // the engine's preview playing, if any
    QList<View> m_views;
    QHash<QString, qsizetype> m_viewIndex;
    QString m_currentId;
};

} // namespace selecta::app
