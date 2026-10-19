// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MainWindow.h"

#include "app/AppSettings.h"
#include "app/Icons.h"
#include "app/Inspector.h"
#include "app/NavModel.h"
#include "app/PlayerBar.h"
#include "app/Sidebar.h"
#include "app/views/EmptyStateView.h"
#include "app/views/HistoryView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolBar>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcUi, "selecta.app.ui")

namespace selecta::app {
namespace {

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr int kMinimumWidth = 900;
constexpr int kMinimumHeight = 560;
constexpr int kSidebarWidth = 220;
constexpr int kInspectorWidth = 300;

const QString kDefaultView = u"get"_s;
const QString kMainSplitter = u"main"_s;
const QString kContentSplitter = u"content"_s;

Appearance nextAppearance(Appearance appearance)
{
    switch (appearance) {
    case Appearance::System:
        return Appearance::Light;
    case Appearance::Light:
        return Appearance::Dark;
    case Appearance::Dark:
        return Appearance::System;
    }
    return Appearance::System;
}

} // namespace

MainWindow::MainWindow(AppServices services, QWidget *parent)
    : QMainWindow(parent)
    , m_services(std::move(services))
    , m_appearance(m_services.settings.appearance())
{
    setObjectName(u"mainWindow"_s);
    setWindowTitle(u"Selecta"_s);
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setUnifiedTitleAndToolBarOnMac(true);

    m_navModel = new NavModel(this);
    createCentralArea();
    createViews();
    createActions();
    createMenus();
    createToolBar();
    restoreSavedState();

    // QCoreApplication::quit() doesn't close windows, so closeEvent isn't enough to keep the window state.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::saveState);
}

MainWindow::~MainWindow() = default;

void MainWindow::createCentralArea()
{
    m_sidebar = new Sidebar(m_navModel, this);
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(u"content"_s);
    m_stack->setAutoFillBackground(true); // content panes use the content colour, the sidebar the window colour
    m_stack->setBackgroundRole(QPalette::Base);
    m_inspector = new Inspector(this);
    m_player = new PlayerBar(this);

    m_contentSplitter = new QSplitter(Qt::Horizontal, this);
    m_contentSplitter->setObjectName(u"contentSplitter"_s);
    m_contentSplitter->addWidget(m_stack);
    m_contentSplitter->addWidget(m_inspector);
    m_contentSplitter->setStretchFactor(0, 1);
    m_contentSplitter->setStretchFactor(1, 0);
    m_contentSplitter->setChildrenCollapsible(false);
    m_contentSplitter->setHandleWidth(1);

    auto *separator = new QFrame(this);
    separator->setObjectName(u"playerSeparator"_s);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setForegroundRole(QPalette::Mid);

    auto *main = new QWidget(this);
    main->setObjectName(u"main"_s);
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(m_contentSplitter, 1);
    mainLayout->addWidget(separator);
    mainLayout->addWidget(m_player);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_mainSplitter->setObjectName(u"mainSplitter"_s);
    m_mainSplitter->addWidget(m_sidebar);
    m_mainSplitter->addWidget(main);
    m_mainSplitter->setStretchFactor(0, 0);
    m_mainSplitter->setStretchFactor(1, 1);
    m_mainSplitter->setChildrenCollapsible(false);
    m_mainSplitter->setHandleWidth(1);
    setCentralWidget(m_mainSplitter);

    connect(m_sidebar, &Sidebar::navigationRequested, this, [this](const QString &id) { navigateTo(id); });
}

void MainWindow::addView(const QString &id, QWidget *widget, std::function<QString()> subtitle)
{
    Q_ASSERT_X(!m_viewIndex.contains(id), "MainWindow::addView", "duplicate view id");
    if (widget->objectName().isEmpty())
        widget->setObjectName(u"view."_s + id);
    m_stack->addWidget(widget);
    m_viewIndex.insert(id, m_views.size());
    m_views.append({id, std::move(subtitle), widget});
}

void MainWindow::createViews()
{
    const auto emptyState = [this](const QString &symbol, const QString &fallback, const QString &title,
                                   const QString &text, const QString &note) {
        return new EmptyStateView(icons::symbol(symbol, fallback), title, text, note, m_stack);
    };
    const auto fixed = [](const QString &text) { return [text] { return text; }; };

    addView(u"get"_s,
            emptyState(u"list.bullet.rectangle"_s, u"view-list-details"_s, tr("Get tracks"),
                       tr("Paste a list of tracks, preview each one with its waveform, and get them as tagged MP3 320 "
                          "files in your Inbox."),
                       tr("Not built yet: arrives with the Soulseek connection (milestone M8).")),
            fixed(tr("Not built yet")));
    addView(u"inbox"_s,
            emptyState(u"tray.and.arrow.down"_s, u"mail-folder-inbox"_s, tr("Your Inbox is empty"),
                       tr("Downloads land here, converted and tagged, until you file them into your collection."),
                       tr("Downloading isn't built yet (milestone M4).")),
            fixed(tr("Nothing waiting")));
    addView(u"wishlist"_s,
            emptyState(u"star"_s, u"starred"_s, tr("No saved searches"),
                       tr("Searches saved here run again on their own and tell you when something new turns up."),
                       tr("Not built yet (milestone M8).")),
            fixed(tr("No saved searches")));
    addView(u"collection"_s,
            emptyState(u"music.note.list"_s, u"folder-music"_s, tr("No collection yet"),
                       tr("Selecta will index your music folders to know what you already own. It never changes "
                          "those files."),
                       tr("Not built yet (milestone M4).")),
            fixed(tr("No folders indexed")));

    auto *history = new HistoryView(m_services.sessions, m_services.build.session, m_stack);
    const QString started =
        QLocale().toString(QDateTime::fromMSecsSinceEpoch(m_services.sessionStartedAtMs).time(), QLocale::ShortFormat);
    addView(u"history"_s, history, fixed(tr("Session %1 started at %2").arg(m_services.build.session).arg(started)));

    addView(u"activity"_s,
            emptyState(u"arrow.up.arrow.down"_s, u"network-transmit-receive"_s, tr("Nothing is transferring"),
                       tr("Downloads, uploads, your ratio and how much you took from each peer today appear here."),
                       tr("Not built yet (milestones M2 and M7).")),
            fixed(tr("Not connected")));
    addView(u"shares"_s,
            emptyState(u"dot.radiowaves.left.and.right"_s, u"folder-publicshare"_s, tr("Nothing shared yet"),
                       tr("Selecta shares your collection, read-only, with other Soulseek users."),
                       tr("Not built yet (milestone M7).")),
            fixed(tr("Nothing shared")));
    addView(u"messages"_s,
            emptyState(u"bubble.left.and.bubble.right"_s, u"mail-message"_s, tr("No conversations"),
                       tr("Private messages with other Soulseek users appear here."),
                       tr("Not built yet (milestone M8).")),
            fixed(tr("No conversations")));
    addView(u"rooms"_s,
            emptyState(u"person.3"_s, u"system-users"_s, tr("No rooms joined"),
                       tr("Join a chat room to read and post."), tr("Not built yet (milestone M8).")),
            fixed(tr("Not connected")));
    addView(u"converter"_s,
            emptyState(u"arrow.triangle.2.circlepath"_s, u"view-refresh"_s, tr("Converter"),
                       tr("Convert between FLAC, MP3, WAV and AIFF, keeping every tag and the cover."),
                       tr("Not built yet (milestone M6).")),
            fixed(tr("Not built yet")));

    m_settingsView = new SettingsView(m_services.build, m_appearance, m_stack);
    connect(m_settingsView, &SettingsView::appearanceRequested, this, &MainWindow::setAppearance);
    addView(u"settings"_s, m_settingsView, fixed(tr("Selecta %1").arg(m_services.build.version)));

#ifndef QT_NO_DEBUG
    for (const QString &id : m_navModel->ids())
        Q_ASSERT_X(m_viewIndex.contains(id), "MainWindow::createViews", "every sidebar item needs a view");
#endif
}

void MainWindow::createActions()
{
    m_toggleSidebar = new QAction(icons::symbol(u"sidebar.left"_s, u"view-left-close"_s), tr("Hide Sidebar"), this);
    m_toggleSidebar->setObjectName(u"action.toggleSidebar"_s);
    m_toggleSidebar->setShortcut(QKeySequence(Qt::CTRL | Qt::META | Qt::Key_S));
    m_toggleSidebar->setToolTip(tr("Show or hide the sidebar"));
    connect(m_toggleSidebar, &QAction::triggered, this, [this] { setSidebarShown(!isSidebarShown()); });

    m_toggleInspector =
        new QAction(icons::symbol(u"sidebar.right"_s, u"view-right-close"_s), tr("Hide Inspector"), this);
    m_toggleInspector->setObjectName(u"action.toggleInspector"_s);
    m_toggleInspector->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_toggleInspector->setToolTip(tr("Show or hide the inspector"));
    connect(m_toggleInspector, &QAction::triggered, this, [this] { setInspectorShown(!isInspectorShown()); });

    m_appearanceGroup = new QActionGroup(this);
    m_appearanceGroup->setExclusive(true);
    const std::array choices{std::pair{Appearance::System, tr("System")}, std::pair{Appearance::Light, tr("Light")},
                             std::pair{Appearance::Dark, tr("Dark")}};
    for (const auto &[value, label] : choices) {
        auto *action = new QAction(label, m_appearanceGroup);
        action->setObjectName(u"action.appearance."_s + appearanceKey(value));
        action->setCheckable(true);
        action->setChecked(value == m_appearance);
        action->setData(static_cast<int>(value));
        connect(action, &QAction::triggered, this, [this, value] { setAppearance(value); });
    }

    const std::array go{std::pair{u"get"_s, Qt::Key_1},        std::pair{u"inbox"_s, Qt::Key_2},
                        std::pair{u"collection"_s, Qt::Key_3}, std::pair{u"history"_s, Qt::Key_4},
                        std::pair{u"activity"_s, Qt::Key_5},   std::pair{u"messages"_s, Qt::Key_6}};
    for (const auto &[id, key] : go) {
        auto *action = new QAction(m_navModel->titleOf(id), this);
        action->setObjectName(u"action.go."_s + id);
        action->setShortcut(QKeySequence(Qt::CTRL | key));
        connect(action, &QAction::triggered, this, [this, viewId = id] { navigateTo(viewId); });
        m_goActions.append(action);
    }
}

void MainWindow::createMenus()
{
    auto *settings = new QAction(tr("Settings…"), this);
    settings->setObjectName(u"action.settings"_s);
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence::Preferences);
    connect(settings, &QAction::triggered, this, [this] { navigateTo(u"settings"_s); });

    auto *about = new QAction(tr("About Selecta"), this);
    about->setObjectName(u"action.about"_s);
    about->setMenuRole(QAction::AboutRole);
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About Selecta"),
                           tr("<b>Selecta %1</b><p>A quality-first Soulseek client for DJs and collectors.</p>"
                              "<p>Free software under the GNU General Public License, version 3 or later.</p>")
                               .arg(m_services.build.version));
    });

    auto *quit = new QAction(tr("Quit Selecta"), this);
    quit->setObjectName(u"action.quit"_s);
    quit->setMenuRole(QAction::QuitRole);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit, Qt::QueuedConnection);

    auto *closeWindow = new QAction(tr("Close Window"), this);
    closeWindow->setObjectName(u"action.closeWindow"_s);
    closeWindow->setShortcut(QKeySequence::Close);
    connect(closeWindow, &QAction::triggered, this, &QWidget::close);

    auto *minimize = new QAction(tr("Minimize"), this);
    minimize->setObjectName(u"action.minimize"_s);
    minimize->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    connect(minimize, &QAction::triggered, this, &QWidget::showMinimized);

    auto *zoom = new QAction(tr("Zoom"), this);
    zoom->setObjectName(u"action.zoom"_s);
    connect(zoom, &QAction::triggered, this, [this] { isMaximized() ? showNormal() : showMaximized(); });

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(closeWindow);
    file->addSeparator();
    file->addAction(settings); // moved to the application menu on macOS, by its role
    file->addAction(about);
    file->addAction(quit);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_toggleSidebar);
    view->addAction(m_toggleInspector);
    view->addSeparator();
    QMenu *appearanceMenu = view->addMenu(tr("Appearance"));
    appearanceMenu->addActions(m_appearanceGroup->actions());

    QMenu *goMenu = menuBar()->addMenu(tr("&Go"));
    goMenu->addActions(m_goActions);

    QMenu *window = menuBar()->addMenu(tr("&Window"));
    window->addAction(minimize);
    window->addAction(zoom);
}

void MainWindow::createToolBar()
{
    QToolBar *bar = addToolBar(tr("Toolbar"));
    bar->setObjectName(u"toolbar"_s);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setIconSize(QSize(18, 18));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);

    bar->addAction(m_toggleSidebar);

    auto *titleBox = new QWidget(bar);
    titleBox->setObjectName(u"toolbar.titleBox"_s);
    auto *titleLayout = new QVBoxLayout(titleBox);
    titleLayout->setContentsMargins(10, 0, 10, 0);
    titleLayout->setSpacing(0);
    m_title = new QLabel(titleBox);
    m_title->setObjectName(u"toolbar.title"_s);
    QFont titleFont = m_title->font();
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_subtitle = new QLabel(titleBox);
    m_subtitle->setObjectName(u"toolbar.subtitle"_s);
    QFont subtitleFont = m_subtitle->font();
    subtitleFont.setPointSizeF(subtitleFont.pointSizeF() - 2.0);
    m_subtitle->setFont(subtitleFont);
    m_subtitle->setForegroundRole(QPalette::PlaceholderText);
    titleLayout->addWidget(m_title);
    titleLayout->addWidget(m_subtitle);
    bar->addWidget(titleBox);

    auto *spacer = new QWidget(bar);
    spacer->setObjectName(u"toolbar.spacer"_s);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    auto *cycleAppearance =
        new QAction(icons::symbol(u"circle.lefthalf.filled"_s, u"weather-clear-night"_s), tr("Appearance"), this);
    cycleAppearance->setObjectName(u"action.cycleAppearance"_s);
    cycleAppearance->setToolTip(tr("Switch between System, Light and Dark"));
    connect(cycleAppearance, &QAction::triggered, this, [this] { setAppearance(nextAppearance(m_appearance)); });
    bar->addAction(cycleAppearance);
    bar->addAction(m_toggleInspector);
}

void MainWindow::restoreSavedState()
{
    AppSettings &settings = m_services.settings;
    if (const QByteArray geometry = settings.windowGeometry(); geometry.isEmpty() || !restoreGeometry(geometry)) {
        QSize size(kDefaultWidth, kDefaultHeight);
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            size = size.boundedTo(available.size() * 0.9);
            move(available.center() - QPoint(size.width() / 2, size.height() / 2));
        }
        resize(size);
    }
    if (!m_mainSplitter->restoreState(settings.splitterState(kMainSplitter)))
        m_mainSplitter->setSizes({kSidebarWidth, std::max(1, width() - kSidebarWidth)});
    if (!m_contentSplitter->restoreState(settings.splitterState(kContentSplitter)))
        m_contentSplitter->setSizes({std::max(1, width() - kSidebarWidth - kInspectorWidth), kInspectorWidth});
    setSidebarShown(settings.sidebarShown());
    setInspectorShown(settings.inspectorShown());
    if (!navigateTo(settings.lastView()))
        navigateTo(kDefaultView);
}

void MainWindow::saveState()
{
    AppSettings &settings = m_services.settings;
    settings.setWindowGeometry(saveGeometry());
    settings.setSplitterState(kMainSplitter, m_mainSplitter->saveState());
    settings.setSplitterState(kContentSplitter, m_contentSplitter->saveState());
    settings.setSidebarShown(isSidebarShown());
    settings.setInspectorShown(isInspectorShown());
    settings.setLastView(m_currentId);
    settings.setAppearance(m_appearance);
    if (!settings.sync())
        qCWarning(lcUi) << "couldn't write settings to" << settings.fileName();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveState();
    event->accept();
}

bool MainWindow::navigateTo(const QString &id)
{
    const auto found = m_viewIndex.constFind(id);
    if (found == m_viewIndex.cend()) {
        if (!id.isEmpty())
            qCWarning(lcUi) << "unknown view" << id;
        return false;
    }
    m_stack->setCurrentIndex(static_cast<int>(found.value()));
    m_currentId = id;
    m_sidebar->select(id);
    refreshTitle();
    Q_EMIT viewChanged(id);
    return true;
}

QStringList MainWindow::viewIds() const
{
    QStringList ids;
    ids.reserve(m_views.size());
    for (const View &view : m_views)
        ids.append(view.id);
    return ids;
}

QString MainWindow::currentTitle() const
{
    return m_title ? m_title->text() : QString();
}

QString MainWindow::currentSubtitle() const
{
    return m_subtitle ? m_subtitle->text() : QString();
}

void MainWindow::refreshTitle()
{
    const auto found = m_viewIndex.constFind(m_currentId);
    if (found == m_viewIndex.cend() || !m_title)
        return;
    const View &view = m_views.at(found.value());
    m_title->setText(m_navModel->titleOf(view.id));
    m_subtitle->setText(view.subtitle ? view.subtitle() : QString());
}

bool MainWindow::isSidebarShown() const
{
    return !m_sidebar->isHidden();
}

void MainWindow::setSidebarShown(bool shown)
{
    m_sidebar->setVisible(shown);
    m_toggleSidebar->setText(shown ? tr("Hide Sidebar") : tr("Show Sidebar"));
}

bool MainWindow::isInspectorShown() const
{
    return !m_inspector->isHidden();
}

void MainWindow::setInspectorShown(bool shown)
{
    m_inspector->setVisible(shown);
    m_toggleInspector->setText(shown ? tr("Hide Inspector") : tr("Show Inspector"));
}

void MainWindow::setAppearance(Appearance appearance)
{
    m_appearance = appearance;
    applyAppearance(appearance);
    m_services.settings.setAppearance(appearance);
    for (QAction *action : m_appearanceGroup->actions()) {
        const QSignalBlocker blocker(action);
        action->setChecked(action->data().toInt() == static_cast<int>(appearance));
    }
    m_settingsView->setAppearance(appearance);
}

} // namespace selecta::app
