// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/Appearance.h"
#include "app/views/SettingsView.h"

#include <QHash>
#include <QList>
#include <QMainWindow>

#include <functional>

class QAction;
class QActionGroup;
class QLabel;
class QSplitter;
class QStackedWidget;

namespace selecta::storage {
class SessionLog;
}

namespace selecta::app {

class AppSettings;
class Inspector;
class NavModel;
class PlayerBar;
class Sidebar;

// What the main window needs from the rest of the app. The referenced objects outlive the window.
struct AppServices
{
    storage::SessionLog &sessions;
    AppSettings &settings;
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
    QActionGroup *m_appearanceGroup = nullptr;
    QList<QAction *> m_goActions;
    QList<View> m_views;
    QHash<QString, qsizetype> m_viewIndex;
    QString m_currentId;
};

} // namespace selecta::app
