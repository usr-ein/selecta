// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MainWindow.h"

#include "app/AppSettings.h"
#include "app/Icons.h"
#include "app/Inspector.h"
#include "app/NavModel.h"
#include "app/Notifications.h"
#include "app/PlayerBar.h"
#include "app/Presets.h"
#include "app/Preview.h"
#include "app/Sidebar.h"
#include "app/Toasts.h"
#include "app/WaveformView.h"
#include "app/views/ActivityView.h"
#include "app/views/BrowseView.h"
#include "app/views/CollectionView.h"
#include "app/views/ConverterInspector.h"
#include "app/views/ConverterView.h"
#include "app/views/GetTracksInspector.h"
#include "app/views/GetTracksView.h"
#include "app/views/HistoryView.h"
#include "app/views/InboxInspector.h"
#include "app/views/InboxView.h"
#include "app/views/MessagesView.h"
#include "app/views/RoomsView.h"
#include "app/views/SearchInspector.h"
#include "app/views/SearchView.h"
#include "app/views/SharesView.h"
#include "app/views/WishlistView.h"
#include "base/Paths.h"
#include "core/EngineHost.h"
#include "governor/Limits.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {
Q_LOGGING_CATEGORY(lcUi, "selecta.app.ui")
} // namespace

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

// Audio the preview cache holds: a peer's file or YouTube's (a Deezer clip lives in memory; a file is the user's).
bool isCachedPreview(const QString &identity)
{
    return identity.startsWith(u"peer|"_s) || identity.startsWith(u"youtube|"_s);
}

} // namespace

MainWindow::MainWindow(AppServices services, QWidget *parent)
    : QMainWindow(parent)
    , m_services(std::move(services))
    , m_appearance(m_services.settings.appearance())
    , m_navModel(new NavModel(this))
{
    setObjectName(u"mainWindow"_s);
    setWindowTitle(u"Selecta"_s);
    setMinimumSize(kMinimumWidth, kMinimumHeight);
    setUnifiedTitleAndToolBarOnMac(true);

    // System notifications and the Dock badge (UI-6). The user is away while this window isn't the active one: Selecta
    // is in the background or its window in the Dock. (With one of Selecta's dialogs in front, macOS keeps a
    // notification quiet anyway, Selecta being the active app.)
    m_notifier = new Notifier(m_services.notifications, [this] { return !isActiveWindow(); }, this);
    m_notifier->setEnabled(m_services.settings.showNotifications());
    connect(m_notifier, &Notifier::badgeChanged, this, [](int count) { qGuiApp->setBadgeNumber(count); });
    connect(m_notifier, &Notifier::activated, this, &MainWindow::showNotificationTarget);

    createCentralArea();
    createViews();
    createNotifications();
    createActions();
    createMenus();
    createToolBar();
    // Tab goes from the sidebar to the view and its inspector, then to the waveform under them (UI-5); the player was
    // built first, for the views to connect to it.
    QWidget *lastOfContent = nullptr;
    for (QWidget *next = m_contentSplitter->nextInFocusChain(); next != this; next = next->nextInFocusChain()) {
        if (m_contentSplitter->isAncestorOf(next) && next->focusPolicy() != Qt::NoFocus) {
            lastOfContent = next;
        }
    }
    if (auto *waveform = m_player->findChild<WaveformView *>(); waveform != nullptr && lastOfContent != nullptr) {
        QWidget::setTabOrder(lastOfContent, waveform);
    }
    restoreSavedState();

    connect(&m_services.engine, &core::EngineHost::pausedChanged, this, [this](bool paused) {
        m_services.settings.setNetworkPaused(paused);
        refreshNetworkState();
    });
    connect(&m_services.engine, &core::EngineHost::searchBudgetChanged, this, &MainWindow::refreshNetworkState);
    connect(&m_services.engine, &core::EngineHost::networkStatusChanged, this, &MainWindow::refreshNetworkState);
    refreshNetworkState();

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
    m_player = new PlayerBar(m_services.preview, this);
    m_player->setVolume(m_services.settings.previewVolume());
    connect(m_player, &PlayerBar::volumeChanged, this,
            [this](double volume) { m_services.settings.setPreviewVolume(volume); });

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

    m_toasts = new Toasts(main);

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
    m_sidebar->setRecentSearches(m_services.settings.recentSearches());
    connect(m_sidebar, &Sidebar::searchRequested, this, [this](const QString &text) {
        m_searchRequest = m_services.engine.openSearch(text);
        m_services.settings.addRecentSearch(text); // SRCH-15
        m_sidebar->setRecentSearches(m_services.settings.recentSearches());
    });
    connect(m_sidebar, &Sidebar::closeRequested, this, [this](const QString &id) {
        if (const qint64 searchId = NavModel::searchOfItem(id); searchId > 0) {
            m_services.engine.closeSearch(searchId);
        }
        if (const QString peer = NavModel::browseOfItem(id); !peer.isEmpty()) {
            m_navModel->removeBrowse(peer);
            m_services.engine.closeBrowse(peer);
            if (m_currentId == id) {
                navigateTo(u"get"_s);
            }
        }
    });
}

void MainWindow::createNotifications()
{
    // The wishlist found files it hadn't seen (SRCH-14).
    connect(&m_services.engine, &core::EngineHost::wishlistFound, this,
            [this](const core::WishlistItem &item, int newFiles) {
                const QString title = newFiles == 1
                                          ? tr("Wishlist: 1 new file for “%1”").arg(item.text)
                                          : tr("Wishlist: %1 new files for “%2”").arg(newFiles).arg(item.text);
                m_toasts->show(title, {}, Toasts::Kind::Good,
                               {{tr("Show"),
                                 [this, item] {
                                     m_services.engine.markWishlistSeen(item.id);
                                     m_searchRequest = m_services.engine.openSearch(item.text);
                                 }}},
                               14s);
            });
    connect(&m_services.engine, &core::EngineHost::albumChanged, this, &MainWindow::albumChanged);
    connect(&m_services.engine, &core::EngineHost::albumReported, this, &MainWindow::albumReported);

    // What also reaches the system while the user is away (UI-6, NET-7); the Notifier decides.
    const core::EngineHost &engine = m_services.engine;
    connect(&engine, &core::EngineHost::messageAdded, m_notifier, &Notifier::messageAdded);
    connect(&engine, &core::EngineHost::roomLineAdded, m_notifier, &Notifier::roomLineAdded);
    connect(&engine, &core::EngineHost::albumReported, m_notifier, &Notifier::albumReported);
    connect(&engine, &core::EngineHost::albumChanged, m_notifier, &Notifier::albumChanged);
    connect(&engine, &core::EngineHost::wishlistFound, m_notifier, &Notifier::wishlistFound);
    connect(&engine, &core::EngineHost::getLinesReset, m_notifier, &Notifier::getLinesReset);
    connect(&engine, &core::EngineHost::getLineChanged, m_notifier, &Notifier::getLineChanged);
    m_notifier->getLinesReset(engine.getLines()); // downloads that resume join the first batch
    // A Get tracks batch that finished: what arrived, and what didn't.
    connect(m_notifier, &Notifier::getTracksFinished, this, [this](const FinishedBatch &batch) {
        const SystemNotification words = Notifier::batchNotification(batch);
        m_toasts->show(words.title, words.body, batch.failed > 0 ? Toasts::Kind::Warning : Toasts::Kind::Good,
                       {{tr("Show"), [this, view = words.target.view] { navigateTo(view); }}});
    });

    // Items the user moves to the Trash go in one notification per gesture, with Undo.
    m_trashedTimer = new QTimer(this);
    m_trashedTimer->setSingleShot(true);
    m_trashedTimer->setInterval(250ms);
    connect(&m_services.engine, &core::EngineHost::inboxItemTrashed, this, [this](const core::TrashedItem &item) {
        m_trashedBatch.append(item);
        m_trashedTimer->start();
    });
    connect(m_trashedTimer, &QTimer::timeout, this, [this] {
        const QList<core::TrashedItem> items = std::exchange(m_trashedBatch, {});
        if (items.isEmpty()) {
            return;
        }
        const QString title =
            items.size() == 1
                ? tr("Moved “%1” to the Trash").arg(QFileInfo(items.constFirst().item.relativePath).fileName())
                : tr("Moved %1 items to the Trash").arg(items.size());
        m_toasts->show(title, {}, Toasts::Kind::Info,
                       {{tr("Undo"), [this, items] { m_services.engine.restoreInboxItems(items); }}}, 9s);
    });
}

void MainWindow::albumChanged(const core::AlbumDownload &album)
{
    // An arrival is told with its report (albumReported()), which comes next; a failure has none.
    if (album.state == core::AlbumDownload::State::Failed) {
        const QString name =
            album.release && !album.release->album.isEmpty() ? album.release->album : album.folderName();
        m_toasts->show(tr("Couldn't download “%1”").arg(name), album.message, Toasts::Kind::Warning);
    }
}

void MainWindow::albumReported(const core::AlbumReport &report)
{
    QStringList lines;
    if (report.arrival) {
        lines.append(report.tracks == 1 ? tr("1 track in Inbox/%1/").arg(report.where)
                                        : tr("%1 tracks in Inbox/%2/").arg(report.tracks).arg(report.where));
        if (!report.warning.isEmpty()) {
            lines.append(report.warning);
        }
    }
    if (!report.trashed.isEmpty() && report.arrival) {
        lines.append(
            report.trashed.size() == 1
                ? tr("“%1” was in your Inbox as a single, so it went to the Trash.").arg(report.trashed.first())
                : tr("%1 singles in your Inbox are on it, so they went to the Trash: %2.")
                      .arg(report.trashed.size())
                      .arg(report.trashed.join(u", "_s)));
    } else if (!report.trashed.isEmpty()) {
        lines.append(tr("They're on “%1”: %2.").arg(report.album, report.trashed.join(u", "_s)));
    }
    if (!report.held.isEmpty()) {
        lines.append(tr("%1 singles in your Inbox are on it: %2. Move them to the Trash?")
                         .arg(report.held.size())
                         .arg(report.held.join(u", "_s)));
    }
    if (!report.flagged.isEmpty()) {
        lines.append(tr("Also on it: %1. Those files stay where they are.").arg(report.flagged.join(u", "_s)));
    }
    QString title;
    if (!report.arrival) {
        title = report.trashed.size() == 1 ? tr("Moved 1 single to the Trash")
                                           : tr("Moved %1 singles to the Trash").arg(report.trashed.size());
    } else {
        title = report.complete ? tr("“%1” is in your Inbox").arg(report.album)
                                : tr("“%1” arrived incomplete").arg(report.album);
    }
    QList<Toasts::Action> actions;
    if (!report.held.isEmpty()) {
        actions.append({tr("Move to Trash"), [this, id = report.albumId] { m_services.engine.confirmSupersede(id); }});
    }
    if (!report.trashed.isEmpty()) {
        actions.append({tr("Undo"), [this, id = report.albumId] { m_services.engine.undoSupersede(id); }});
    }
    const Toasts::Kind kind = report.arrival && !report.complete ? Toasts::Kind::Warning : Toasts::Kind::Good;
    // A question waits longer than news, and news with Undo longer than plain news.
    std::chrono::milliseconds timeout = Toasts::kTimeout;
    if (!report.held.isEmpty()) {
        timeout = 60s;
    } else if (!report.trashed.isEmpty()) {
        timeout = 14s;
    }
    m_toasts->show(title, lines.join(u'\n'), kind, actions, timeout);
}

void MainWindow::addView(const QString &id, QWidget *widget, std::function<QString()> subtitle)
{
    Q_ASSERT_X(!m_viewIndex.contains(id), "MainWindow::addView", "duplicate view id");
    if (widget->objectName().isEmpty()) {
        widget->setObjectName(u"view."_s + id);
    }
    m_stack->addWidget(widget);
    m_viewIndex.insert(id, m_views.size());
    m_views.append({id, std::move(subtitle), widget});
}

void MainWindow::createViews()
{
    // The filter presets Search, Get tracks and the Wishlist share (SRCH-8).
    m_presets = new Presets(m_services.settings, this);

    const auto fixed = [](const QString &text) { return [text] { return text; }; };

    createSearchView();
    createBrowseView();

    m_getTracksView = new GetTracksView(m_services.engine, *m_presets, m_stack);
    addView(u"get"_s, m_getTracksView, [this] { return m_getTracksView->summary(); });
    m_inspector->addPanel(u"get"_s, m_getTracksView->inspectorPanel());
    connect(m_getTracksView, &GetTracksView::summaryChanged, this, [this] {
        if (m_currentId == u"get"_s) {
            refreshTitle();
        }
    });
    m_inboxView = new InboxView(m_services.engine, m_stack);
    m_inboxView->setLibraryConfig(m_services.settings.libraryConfig());
    m_inboxView->setAnalysisCache(m_services.preview.sharedAnalysisCache());
    addView(u"inbox"_s, m_inboxView, [this] { return m_inboxView->summary(); });
    m_inspector->addPanel(u"inbox"_s, m_inboxView->inspectorPanel());
    connect(m_inboxView, &InboxView::summaryChanged, this, [this] {
        if (m_currentId == u"inbox"_s) {
            refreshTitle();
        }
    });
    connect(m_inboxView, &InboxView::previewRequested, this, [this] { previewInboxSelection(false); });
    connect(m_inboxView, &InboxView::convertRequested, this, [this](const QStringList &paths) {
        if (m_converterView) {
            m_converterView->addPaths(paths);
            navigateTo(u"converter"_s);
        }
    });
    connect(m_getTracksView, &GetTracksView::previewRequested, this,
            [this] { previewGetTracksLine(false, m_services.settings.previewSource()); });
    connect(m_getTracksView, &GetTracksView::selectionMoved, this, &MainWindow::selectionMoved);
    connect(m_inboxView, &InboxView::selectionMoved, this, &MainWindow::selectionMoved);
    connect(&m_services.engine, &core::EngineHost::previewStarted, this,
            [this](const QString &key, qint64 previewId, const std::shared_ptr<audio::ByteSource> &source,
                   const QString &identity, const QString &title, const QString &artist, const QString &label) {
                if (key != m_askedPreviewKey) {
                    return; // the user asked for something else meanwhile
                }
                m_previewId = previewId;
                m_playingKey = key;
                m_playingIdentity = identity;
                m_services.preview.preview({source, {title, artist, label}, identity});
                refreshPreviewActions();
            });
    connect(&m_services.engine, &core::EngineHost::previewRefused, this,
            [this](const QString &key, const QString &why) {
                if (key == m_askedPreviewKey) {
                    m_askedPreviewKey.clear();
                    m_player->showMessage(why);
                }
            });
    connect(&m_services.engine, &core::EngineHost::previewChanged, this, [this](const core::Preview &preview) {
        // While a preview is being looked for, each source says what it's doing (YouTube, then the clip…).
        if (!m_askedPreviewKey.isEmpty() && m_askedPreviewKey != m_playingKey && !preview.isFinished()) {
            m_player->showMessage(preview.message);
            return;
        }
        if (preview.id != m_previewId) {
            return;
        }
        // While nothing plays yet, say what the peer is doing; a failure always.
        const bool waiting = m_services.preview.player().state() == playback::Player::State::Loading;
        if (preview.state == core::Preview::State::Failed || (waiting && !preview.isFinished())) {
            m_player->showMessage(preview.message);
        }
    });
    auto *wishlistView = new WishlistView(m_services.engine, *m_presets, m_stack);
    addView(u"wishlist"_s, wishlistView, [wishlistView] { return wishlistView->summary(); });
    connect(wishlistView, &WishlistView::summaryChanged, this, [this, wishlistView] {
        m_navModel->setCount(u"wishlist"_s, wishlistView->foundCount());
        if (m_currentId == u"wishlist"_s) {
            refreshTitle();
        }
    });
    connect(wishlistView, &WishlistView::searchRequested, this,
            [this](const QString &text) { m_searchRequest = m_services.engine.openSearch(text); });
    m_navModel->setCount(u"wishlist"_s, wishlistView->foundCount());
    auto *collectionView = new CollectionView(m_services.engine, m_services.database, m_stack);
    addView(u"collection"_s, collectionView, [collectionView] { return collectionView->summary(); });
    connect(collectionView, &CollectionView::summaryChanged, this, [this] {
        if (m_currentId == u"collection"_s) {
            refreshTitle();
        }
    });
    connect(collectionView, &CollectionView::settingsRequested, this, [this] { navigateTo(u"settings"_s); });
    connect(collectionView, &CollectionView::inboxRequested, this, [this] { navigateTo(u"inbox"_s); });

    auto *history = new HistoryView(m_services.database, m_services.build.session, m_stack);
    // Filing and trashing change where downloads are; arrivals add new ones.
    connect(&m_services.engine, &core::EngineHost::inboxChanged, history, [history] {
        if (history->isVisible()) {
            history->refresh();
        }
    });
    const QString started =
        QLocale().toString(QDateTime::fromMSecsSinceEpoch(m_services.sessionStartedAtMs).time(), QLocale::ShortFormat);
    addView(u"history"_s, history, fixed(tr("Session %1 started at %2").arg(m_services.build.session).arg(started)));

    m_activityView = new ActivityView(m_services.engine, m_services.database, m_stack);
    connect(m_activityView, &ActivityView::browseRequested, this, &MainWindow::browsePeer);
    connect(m_activityView, &ActivityView::messageRequested, this, &MainWindow::messagePeer);
    addView(u"activity"_s, m_activityView, [this] { return m_activityView->summary(); });
    connect(m_activityView, &ActivityView::summaryChanged, this, [this] {
        if (m_currentId == u"activity"_s) {
            refreshTitle();
        }
    });
    auto *sharesView = new SharesView(m_services.engine, m_stack);
    addView(u"shares"_s, sharesView, [sharesView] { return sharesView->summary(); });
    connect(sharesView, &SharesView::summaryChanged, this, [this] {
        if (m_currentId == u"shares"_s) {
            refreshTitle();
        }
    });
    connect(sharesView, &SharesView::settingsRequested, this, [this] { navigateTo(u"settings"_s); });
    connect(sharesView, &SharesView::browseRequested, this, &MainWindow::browsePeer);
    connect(sharesView, &SharesView::messageRequested, this, &MainWindow::messagePeer);
    createMessagesView();
    createRoomsView();
    m_converterView = new ConverterView(m_services.engine, m_services.settings, m_stack);
    addView(u"converter"_s, m_converterView, [this] { return m_converterView->summary(); });
    m_inspector->addPanel(u"converter"_s, m_converterView->inspectorPanel());
    connect(m_converterView, &ConverterView::summaryChanged, this, [this] {
        if (m_currentId == u"converter"_s) {
            refreshTitle();
        }
    });

    createSettingsView();
    addView(u"settings"_s, m_settingsView, fixed(tr("Selecta %1").arg(m_services.build.version)));

#ifndef QT_NO_DEBUG
    const QStringList ids = m_navModel->ids();
    for (const QString &id : ids) {
        Q_ASSERT_X(m_viewIndex.contains(id), "MainWindow::createViews", "every sidebar item needs a view");
    }
#endif
}

void MainWindow::createSearchView()
{
    // Searches typed by hand: one view shows whichever search is picked in the sidebar.
    m_searchView = new SearchView(m_services.engine, m_services.settings, *m_presets, m_stack);
    addView(u"search"_s, m_searchView, [this] { return m_searchView->summary(); });
    m_inspector->addPanel(u"search"_s, m_searchView->inspectorPanel());
    connect(m_searchView, &SearchView::summaryChanged, this, [this] {
        if (NavModel::searchOfItem(m_currentId) > 0) {
            refreshTitle();
        }
    });
    connect(m_searchView, &SearchView::previewRequested, this,
            [this] { previewSearchResult(false, m_services.settings.previewSource()); });
    connect(m_searchView, &SearchView::selectionMoved, this, &MainWindow::selectionMoved);
    connect(m_searchView, &SearchView::browseRequested, this, &MainWindow::browsePeer);
    connect(m_searchView, &SearchView::messageRequested, this, &MainWindow::messagePeer);
    connect(&m_services.engine, &core::EngineHost::searchOpened, this, [this](quint64 request, qint64 searchId) {
        if (request == m_searchRequest && searchId > 0) {
            if (const std::optional<core::ManualSearch> search = m_services.engine.search(searchId)) {
                showSearchInSidebar(*search);
            }
            navigateTo(NavModel::searchItemId(searchId));
        }
    });
    connect(&m_services.engine, &core::EngineHost::manualSearchChanged, this, &MainWindow::showSearchInSidebar);
    connect(&m_services.engine, &core::EngineHost::manualSearchClosed, this, [this](qint64 searchId) {
        const bool current = NavModel::searchOfItem(m_currentId) == searchId;
        m_navModel->removeSearch(searchId);
        if (current) {
            // Another open search, or the home screen.
            const QList<core::ManualSearch> searches = m_services.engine.searches();
            navigateTo(searches.isEmpty() ? kDefaultView : NavModel::searchItemId(searches.constLast().id));
        }
    });
}

void MainWindow::createBrowseView()
{
    m_browseView = new BrowseView(m_services.engine, m_services.settings, m_stack);
    addView(u"browse"_s, m_browseView, [this] { return m_browseView->summary(); });
    connect(m_browseView, &BrowseView::summaryChanged, this, [this] {
        if (!NavModel::browseOfItem(m_currentId).isEmpty()) {
            refreshTitle();
        }
    });
    connect(m_browseView, &BrowseView::messageRequested, this, &MainWindow::messagePeer);
    connect(m_browseView, &BrowseView::previewRequested, this,
            [this] { previewBrowsedFile(false, m_services.settings.previewSource()); });
    connect(m_browseView, &BrowseView::selectionMoved, this, &MainWindow::selectionMoved);
    connect(&m_services.engine, &core::EngineHost::browseChanged, this, [this](const core::Browse &browse) {
        const bool busy =
            browse.state == core::Browse::State::Requesting || browse.state == core::Browse::State::Receiving;
        m_navModel->setBrowse(browse.peer, browse.files, busy);
    });
    connect(&m_services.engine, &core::EngineHost::browseClosed, this, [this](const QString &peer) {
        m_navModel->removeBrowse(peer);
        if (NavModel::browseOfItem(m_currentId) == peer) {
            navigateTo(u"get"_s);
        }
    });
}

void MainWindow::browsePeer(const QString &peer)
{
    if (peer.isEmpty()) {
        return;
    }
    m_navModel->setBrowse(peer, 0, true);
    m_services.engine.browse(peer);
    navigateTo(NavModel::browseItemId(peer));
}

void MainWindow::createMessagesView()
{
    // Private messages and the user lists (NET-7, NET-8), with a notification for a message nobody is reading.
    m_messagesView = new MessagesView(m_services.engine, m_stack);
    addView(u"messages"_s, m_messagesView, [this] { return m_messagesView->summary(); });
    connect(m_messagesView, &MessagesView::summaryChanged, this, [this] {
        if (m_currentId == u"messages"_s) {
            refreshTitle();
        }
    });
    connect(m_messagesView, &MessagesView::browseRequested, this, &MainWindow::browsePeer);
    connect(&m_services.engine, &core::EngineHost::conversationsChanged, this, &MainWindow::refreshUnread);
    connect(&m_services.engine, &core::EngineHost::messageAdded, this, [this](const core::ChatMessage &message) {
        if (message.outgoing || m_messagesView->isReading(message.peer)) {
            return;
        }
        constexpr qsizetype kShownLength = 200;
        const QString text =
            message.text.size() > kShownLength ? message.text.left(kShownLength - 1) + u'…' : message.text;
        m_toasts->show(tr("Message from %1").arg(message.peer), text, Toasts::Kind::Info,
                       {{tr("Reply"), [this, peer = message.peer] { messagePeer(peer); }}});
        QApplication::alert(this); // the Dock icon bounces once while Selecta is in the background
    });
    refreshUnread();
}

void MainWindow::createRoomsView()
{
    // Minimal chat rooms (NET-10): unread lines in the sidebar, a notification when someone names you.
    m_roomsView = new RoomsView(m_services.engine, m_stack);
    addView(u"rooms"_s, m_roomsView, [this] { return m_roomsView->summary(); });
    connect(m_roomsView, &RoomsView::summaryChanged, this, [this] {
        if (m_currentId == u"rooms"_s) {
            refreshTitle();
        }
    });
    connect(m_roomsView, &RoomsView::unreadChanged, this,
            [this] { m_navModel->setCount(u"rooms"_s, m_roomsView->unread()); });
    connect(m_roomsView, &RoomsView::browseRequested, this, &MainWindow::browsePeer);
    connect(m_roomsView, &RoomsView::messageRequested, this, &MainWindow::messagePeer);
    connect(&m_services.engine, &core::EngineHost::roomLineAdded, this,
            [this](const QString &room, const core::RoomLine &line) {
                if (!line.mention || m_roomsView->isReading(room)) {
                    return;
                }
                m_toasts->show(tr("%1 mentioned you in %2").arg(line.user, room), line.text, Toasts::Kind::Info,
                               {{tr("Show"), [this, room] {
                                     navigateTo(u"rooms"_s);
                                     m_roomsView->openRoom(room);
                                 }}});
            });
}

void MainWindow::refreshUnread()
{
    const int unread = m_messagesView->unread();
    m_navModel->setCount(u"messages"_s, unread);
    m_notifier->setUnreadMessages(unread); // the Dock icon counts them, with what arrived while away (UI-6)
}

void MainWindow::showNotificationTarget(const NotificationTarget &target)
{
    // Selecta came forward (the sink made it the active app); its window follows, even from the Dock. A window kept off
    // the screen (headless runs) stays where it is: coming forward would take the keyboard from whatever the user is
    // doing, for a window nobody can see.
    if (!testAttribute(Qt::WA_DontShowOnScreen)) {
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        raise();
        activateWindow();
    }
    if (target.view == u"messages"_s && !target.argument.isEmpty()) {
        messagePeer(target.argument);
    } else if (target.view == u"rooms"_s && !target.argument.isEmpty()) {
        navigateTo(u"rooms"_s);
        m_roomsView->openRoom(target.argument);
    } else if (!navigateTo(target.view)) {
        qCWarning(lcUi) << "a notification asked for an unknown view" << target.view;
    }
}

void MainWindow::messagePeer(const QString &peer)
{
    if (peer.isEmpty() || !m_messagesView) {
        return;
    }
    navigateTo(u"messages"_s);
    m_messagesView->openConversation(peer);
}

void MainWindow::previewBrowsedFile(bool toggleIfLoaded, core::PreviewSource source)
{
    const std::optional<int> index = m_browseView->previewTarget();
    if (!index) {
        return;
    }
    PreviewController &preview = m_services.preview;
    const playback::Player::State state = preview.player().state();
    const bool loaded = preview.player().isActive() || state == playback::Player::State::Ended;
    const QString key = core::previewKeyForBrowsed(m_browseView->peer(), *index);
    if (toggleIfLoaded && loaded && key == m_playingKey) {
        preview.toggle();
        return;
    }
    m_askedPreviewKey = key;
    m_player->showMessage(askingMessage(source, m_browseView->peer()));
    m_services.engine.previewBrowsed(m_browseView->peer(), *index, source);
}

void MainWindow::createSettingsView()
{
    m_settingsView = new SettingsView(m_services.build, m_appearance, m_services.settings.libraryConfig(),
                                      m_services.settings.networkConfig(), m_stack);
    // Caches (CACHE-2): sizes when Settings opens, and after each is emptied.
    const auto showCaches = [this](const core::CacheStatus &status) {
        audio::AnalysisCache *waveforms = m_services.preview.analysisCache();
        m_settingsView->setCacheSizes(status, waveforms ? waveforms->totalBytes() : 0,
                                      waveforms ? waveforms->budgetBytes() : 0);
    };
    connect(&m_services.engine, &core::EngineHost::cachesChanged, this, showCaches);
    connect(this, &MainWindow::viewChanged, this, [this](const QString &id) {
        if (id == u"settings"_s) {
            m_services.engine.refreshCaches();
        }
    });
    connect(m_settingsView, &SettingsView::clearSearchCacheRequested, &m_services.engine,
            &core::EngineHost::clearSearchCache);
    connect(m_settingsView, &SettingsView::clearPreviewCacheRequested, &m_services.engine,
            &core::EngineHost::clearPreviewCache);
    connect(m_settingsView, &SettingsView::clearWaveformCacheRequested, this, [this, showCaches] {
        if (audio::AnalysisCache *waveforms = m_services.preview.analysisCache()) {
            waveforms->clear();
        }
        showCaches(m_services.engine.cacheStatus());
    });
    connect(m_settingsView, &SettingsView::appearanceRequested, this, &MainWindow::setAppearance);
    connect(m_settingsView, &SettingsView::libraryConfigChanged, this, [this](const core::LibraryConfig &config) {
        if (!base::paths::ensureDir(config.inboxRoot)) {
            qCWarning(lcUi) << "can't create the Inbox folder" << config.inboxRoot;
        }
        m_services.settings.setLibraryConfig(config);
        if (!m_services.settings.sync()) {
            qCWarning(lcUi) << "couldn't write settings to" << m_services.settings.fileName();
        }
        m_services.engine.setLibraryConfig(config);
        m_inboxView->setLibraryConfig(config);
    });
    // META-4, D30: the notice on Spotify's terms is answered once; the engine uses Spotify only after a yes.
    m_settingsView->setSpotifyTermsAccepted(m_services.settings.spotifyTermsAccepted());
    connect(m_settingsView, &SettingsView::spotifyTermsAccepted, this, [this] {
        m_services.settings.setSpotifyTermsAccepted(true);
        if (!m_services.settings.sync()) {
            qCWarning(lcUi) << "couldn't write settings to" << m_services.settings.fileName();
        }
        m_services.engine.allowSpotify();
    });
    connect(m_settingsView, &SettingsView::spotifyCredentialsSubmitted, &m_services.engine,
            &core::EngineHost::setSpotifyCredentials);
    connect(m_settingsView, &SettingsView::spotifySignInRequested, &m_services.engine,
            &core::EngineHost::signInToSpotify);
    connect(m_settingsView, &SettingsView::spotifySignOutRequested, &m_services.engine,
            &core::EngineHost::signOutOfSpotify);
    connect(&m_services.engine, &core::EngineHost::spotifyStatusChanged, m_settingsView,
            &SettingsView::setSpotifyStatus);
    m_settingsView->setSpotifyStatus(m_services.engine.spotifyStatus());
    connect(m_settingsView, &SettingsView::networkConfigChanged, this, [this](const core::NetworkConfig &config) {
        m_services.settings.setNetworkConfig(config);
        if (!m_services.settings.sync()) {
            qCWarning(lcUi) << "couldn't write settings to" << m_services.settings.fileName();
        }
        m_services.engine.setNetworkConfig(config);
    });
    connect(m_settingsView, &SettingsView::networkPasswordSubmitted, &m_services.engine,
            &core::EngineHost::setNetworkPassword);
    connect(m_settingsView, &SettingsView::connectRequested, &m_services.engine, &core::EngineHost::connectNetwork);
    m_settingsView->setShowNotifications(m_services.settings.showNotifications());
    connect(m_settingsView, &SettingsView::showNotificationsChanged, this, [this](bool show) {
        m_services.settings.setShowNotifications(show);
        m_notifier->setEnabled(show);
    });
    m_settingsView->setPreviewSettings(m_services.settings.previewSource(), m_services.settings.ytDlpPath());
    connect(m_settingsView, &SettingsView::previewSourceChanged, this,
            [this](core::PreviewSource source) { m_services.settings.setPreviewSource(source); });
    connect(m_settingsView, &SettingsView::ytDlpPathChanged, this, [this](const QString &path) {
        m_services.settings.setYtDlpPath(path);
        m_services.engine.setYtDlpPath(path);
    });
    connect(m_settingsView, &SettingsView::ytDlpCheckRequested, &m_services.engine, &core::EngineHost::checkYtDlp);
    connect(&m_services.engine, &core::EngineHost::ytDlpChecked, m_settingsView, &SettingsView::setYtDlpStatus);
    connect(m_settingsView, &SettingsView::disconnectRequested, &m_services.engine,
            &core::EngineHost::disconnectNetwork);
    connect(&m_services.engine, &core::EngineHost::networkStatusChanged, m_settingsView,
            &SettingsView::setNetworkStatus);
    m_settingsView->setNetworkStatus(m_services.engine.networkStatus());
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

    // "Pause network activity" (SAFE-5). Checked means paused; the engine confirms through pausedChanged.
    m_togglePause = new QAction(tr("Pause Network Activity"), this);
    m_togglePause->setObjectName(u"action.togglePause"_s);
    m_togglePause->setCheckable(true);
    m_togglePause->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(m_togglePause, &QAction::triggered, this, [this](bool paused) { m_services.engine.setPaused(paused); });

    auto *find = new QAction(tr("Search Soulseek…"), this);
    find->setObjectName(u"action.find"_s);
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered, this, [this] {
        setSidebarShown(true);
        m_sidebar->focusSearch();
    });
    m_findAction = find;

    m_budget = new QAction(icons::symbol(u"magnifyingglass"_s, u"edit-find"_s), QString(), this);
    m_budget->setObjectName(u"action.searchBudget"_s);
    connect(m_budget, &QAction::triggered, this, [this] { navigateTo(u"activity"_s); });

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

    // Space plays and pauses wherever focus is, except in text fields, which keep it for typing (they claim it
    // through ShortcutOverride). It isn't a menu shortcut: a menu's key equivalent would take it from them.
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    space->setObjectName(u"shortcut.playPause"_s);
    space->setContext(Qt::WindowShortcut);
    connect(space, &QShortcut::activated, this, &MainWindow::playOrPause);
    auto *shiftSpace = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Space), this);
    shiftSpace->setObjectName(u"shortcut.previewPeerFile"_s);
    shiftSpace->setContext(Qt::WindowShortcut);
    connect(shiftSpace, &QShortcut::activated, this, &MainWindow::previewPeerFile);
    auto *keep = new QShortcut(QKeySequence(Qt::Key_K), this);
    keep->setObjectName(u"shortcut.keepPreview"_s);
    keep->setContext(Qt::WindowShortcut);
    connect(keep, &QShortcut::activated, this, &MainWindow::keepPreview);
    // D, beside K: ⌫ (the mock's key) already removes the selected rows in Get tracks and the Wishlist.
    auto *discard = new QShortcut(QKeySequence(Qt::Key_D), this);
    discard->setObjectName(u"shortcut.discardPreview"_s);
    discard->setContext(Qt::WindowShortcut);
    connect(discard, &QShortcut::activated, this, &MainWindow::discardPreview);

    playback::Player &player = m_services.preview.player();
    const std::array skips{
        std::tuple{u"back5"_s, tr("Back 5 Seconds"), -5.0, QKeySequence(Qt::ALT | Qt::Key_Left)},
        std::tuple{u"forward5"_s, tr("Forward 5 Seconds"), 5.0, QKeySequence(Qt::ALT | Qt::Key_Right)},
        std::tuple{u"back30"_s, tr("Back 30 Seconds"), -30.0, QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left)},
        std::tuple{u"forward30"_s, tr("Forward 30 Seconds"), 30.0, QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right)}};
    auto *playPause = new QAction(tr("Play or Pause"), this);
    playPause->setObjectName(u"action.playPause"_s);
    connect(playPause, &QAction::triggered, this, &MainWindow::playOrPause);
    m_playbackActions.append(playPause);
    for (const auto &[name, text, seconds, keys] : skips) {
        auto *action = new QAction(text, this);
        action->setObjectName(u"action."_s + name);
        action->setShortcut(keys);
        connect(action, &QAction::triggered, &player, [&player, step = seconds] {
            if (player.isActive()) {
                player.skip(step);
            }
        });
        m_playbackActions.append(action);
    }
    auto *peerFile = new QAction(tr("Preview the Peer's File"), this);
    peerFile->setObjectName(u"action.previewPeerFile"_s);
    peerFile->setToolTip(tr("Hear the very file a Get would download (⇧Space)"));
    connect(peerFile, &QAction::triggered, this, &MainWindow::previewPeerFile);
    m_playbackActions.append(peerFile);
    auto *keepAction = new QAction(tr("Keep Preview"), this);
    keepAction->setObjectName(u"action.keepPreview"_s);
    keepAction->setToolTip(tr("Put the peer's file you're hearing into your Inbox, without downloading it again (K)"));
    connect(keepAction, &QAction::triggered, this, &MainWindow::keepPreview);
    m_playbackActions.append(keepAction);
    m_discardPreview = new QAction(icons::symbol(u"trash"_s, u"edit-delete"_s), tr("Discard Preview"), this);
    m_discardPreview->setObjectName(u"action.discardPreview"_s);
    m_discardPreview->setToolTip(tr("Delete the preview you're hearing from the preview cache (D)"));
    connect(m_discardPreview, &QAction::triggered, this, &MainWindow::discardPreview);
    m_playbackActions.append(m_discardPreview);
    m_player->addPreviewAction(m_discardPreview, u"player.discard"_s);
    m_followSelection = new QAction(tr("Preview as You Move"), this);
    m_followSelection->setObjectName(u"action.followSelection"_s);
    m_followSelection->setCheckable(true);
    m_followSelection->setChecked(m_services.settings.previewFollowsSelection());
    connect(m_followSelection, &QAction::toggled, this,
            [this](bool follows) { m_services.settings.setPreviewFollowsSelection(follows); });
    m_playbackActions.append(m_followSelection);
    m_followTimer = new QTimer(this);
    m_followTimer->setSingleShot(true);
    m_followTimer->setInterval(governor::limits::kAutoPreviewRest);
    connect(m_followTimer, &QTimer::timeout, this,
            [this] { previewSelection(false, m_services.settings.previewSource()); });

    auto *stop = new QAction(tr("Stop"), this);
    stop->setObjectName(u"action.stopPreview"_s);
    stop->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
    connect(stop, &QAction::triggered, this, [this] {
        m_services.preview.stop();
        m_askedPreviewKey.clear();
        m_playingKey.clear();
        m_playingIdentity.clear();
        if (m_previewId != 0) {
            m_services.engine.stopPreview(); // a nearly complete preview still finishes, for a Get to reuse
            m_previewId = 0;
        }
        refreshPreviewActions();
    });
    m_playbackActions.append(stop);
    refreshPreviewActions();

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

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_findAction);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_toggleSidebar);
    view->addAction(m_toggleInspector);
    view->addSeparator();
    QMenu *appearanceMenu = view->addMenu(tr("Appearance"));
    appearanceMenu->addActions(m_appearanceGroup->actions());

    QMenu *goMenu = menuBar()->addMenu(tr("&Go"));
    goMenu->addActions(m_goActions);

    QMenu *playback = menuBar()->addMenu(tr("&Playback"));
    for (QAction *action : std::as_const(m_playbackActions)) {
        playback->addAction(action);
        if (action->objectName() == u"action.playPause"_s || action->objectName() == u"action.forward30"_s) {
            playback->addSeparator();
        }
    }

    QMenu *network = menuBar()->addMenu(tr("&Network"));
    network->addAction(m_togglePause);

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

    auto *budgetButton = new QToolButton(bar);
    budgetButton->setObjectName(u"toolbar.budget"_s);
    budgetButton->setDefaultAction(m_budget);
    budgetButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    budgetButton->setAutoRaise(true);
    bar->addWidget(budgetButton);
    // The ratio, always in sight (FAIR-3): what went out against what came in, over 30 days.
    m_ratio = new QAction(icons::symbol(u"arrow.up.arrow.down"_s, u"network-transmit-receive"_s), QString(), this);
    m_ratio->setObjectName(u"action.ratio"_s);
    connect(m_ratio, &QAction::triggered, this, [this] { navigateTo(u"shares"_s); });
    auto *ratioButton = new QToolButton(bar);
    ratioButton->setObjectName(u"toolbar.ratio"_s);
    ratioButton->setDefaultAction(m_ratio);
    ratioButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ratioButton->setAutoRaise(true);
    bar->addWidget(ratioButton);
    const auto showRatio = [this](const core::ShareStatus &status) {
        const core::RatioAssessment ratio = core::assessRatio(status);
        m_ratio->setText(ratio.level == core::RatioAssessment::Level::Low ? ratio.summary + u" !"_s : ratio.summary);
        m_ratio->setToolTip(ratio.guidance);
    };
    connect(&m_services.engine, &core::EngineHost::shareStatusChanged, this, showRatio);
    showRatio(m_services.engine.shareStatus());
    bar->addAction(m_togglePause);

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
    const AppSettings &settings = m_services.settings;
    if (const QByteArray geometry = settings.windowGeometry(); geometry.isEmpty() || !restoreGeometry(geometry)) {
        QSize size(kDefaultWidth, kDefaultHeight);
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            size = size.boundedTo(available.size() * 0.9);
            move(available.center() - QPoint(size.width() / 2, size.height() / 2));
        }
        resize(size);
    }
    if (!m_mainSplitter->restoreState(settings.splitterState(kMainSplitter))) {
        m_mainSplitter->setSizes({kSidebarWidth, std::max(1, width() - kSidebarWidth)});
    }
    if (!m_contentSplitter->restoreState(settings.splitterState(kContentSplitter))) {
        m_contentSplitter->setSizes({std::max(1, width() - kSidebarWidth - kInspectorWidth), kInspectorWidth});
    }
    setSidebarShown(settings.sidebarShown());
    setInspectorShown(settings.inspectorShown());
    if (!navigateTo(settings.lastView())) {
        navigateTo(kDefaultView);
    }
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
    if (!settings.sync()) {
        qCWarning(lcUi) << "couldn't write settings to" << settings.fileName();
    }
}

void MainWindow::playOrPause()
{
    previewSelection(true, m_services.settings.previewSource());
}

void MainWindow::previewPeerFile()
{
    previewSelection(true, core::PreviewSource::Peer);
}

void MainWindow::previewSelection(bool toggleIfLoaded, core::PreviewSource source)
{
    if (m_currentId == u"inbox"_s && m_inboxView->previewTarget()) {
        previewInboxSelection(toggleIfLoaded);
        return;
    }
    if (m_currentId == u"get"_s && m_getTracksView->previewTarget()) {
        previewGetTracksLine(toggleIfLoaded, source);
        return;
    }
    if (NavModel::searchOfItem(m_currentId) > 0 && m_searchView->previewTarget()) {
        previewSearchResult(toggleIfLoaded, source);
        return;
    }
    if (!NavModel::browseOfItem(m_currentId).isEmpty() && m_browseView->previewTarget()) {
        previewBrowsedFile(toggleIfLoaded, source);
        return;
    }
    if (toggleIfLoaded) {
        m_services.preview.toggle();
    }
}

QString MainWindow::askingMessage(core::PreviewSource source, const QString &peer)
{
    switch (source) {
    case core::PreviewSource::Automatic:
    case core::PreviewSource::YouTube:
        return tr("Looking for it on YouTube…");
    case core::PreviewSource::Clip:
        return tr("Looking for its 30-second clip…");
    case core::PreviewSource::Peer:
        break;
    }
    return peer.isEmpty() ? tr("No file on Soulseek to preview yet.") : tr("Asking %1 for a preview…").arg(peer);
}

void MainWindow::keepPreview()
{
    if (m_playingKey.isEmpty() || !m_playingIdentity.startsWith(u"peer|"_s)) {
        m_player->showMessage(m_playingKey.isEmpty()
                                  ? tr("Nothing to keep: preview a file first.")
                                  : tr("Only a peer's file can be kept; previews from YouTube and Deezer are for "
                                       "listening. ⇧Space previews the peer's file."));
        return;
    }
    // A Get of the very file: the preview's bytes go to the Inbox, nothing is downloaded again.
    const QStringList parts = m_playingKey.split(u':');
    if (parts.size() == 2 && parts.at(0) == u"line"_s) {
        m_services.engine.downloadTracks({parts.at(1).toLongLong()});
    } else if (parts.size() == 3 && parts.at(0) == u"search"_s) {
        m_services.engine.downloadResults(parts.at(1).toLongLong(), {parts.at(2).toInt()});
    } else {
        return;
    }
    m_player->showMessage(tr("Kept: it goes to your Inbox once it has arrived."));
}

void MainWindow::discardPreview()
{
    if (m_previewId == 0 || !isCachedPreview(m_playingIdentity)) {
        if (m_playingIdentity.startsWith(u"file|"_s)) {
            m_player->showMessage(tr("Only previews can be discarded: this file of yours stays where it is."));
        } else if (m_playingIdentity.startsWith(u"clip|"_s)) {
            m_player->showMessage(tr("Deezer's 30-second clips aren't kept, so there's nothing to discard."));
        } else {
            m_player->showMessage(tr("Nothing to discard: preview a file first."));
        }
        return;
    }
    const qint64 previewId = std::exchange(m_previewId, 0);
    m_services.preview.stop();
    m_askedPreviewKey.clear();
    m_playingKey.clear();
    m_playingIdentity.clear();
    m_services.engine.discardPreview(previewId); // a transfer still under way stops (PREV-6)
    m_player->showMessage(tr("Discarded: it's gone from the preview cache."));
    refreshPreviewActions();
}

void MainWindow::refreshPreviewActions()
{
    m_discardPreview->setEnabled(m_previewId != 0 && isCachedPreview(m_playingIdentity));
}

void MainWindow::selectionMoved()
{
    if (m_followSelection && m_followSelection->isChecked()) {
        m_followTimer->start(); // a rest first: running down a list doesn't start a preview per row
    }
}

void MainWindow::previewSearchResult(bool toggleIfLoaded, core::PreviewSource source)
{
    const std::optional<SearchView::PreviewTarget> target = m_searchView->previewTarget();
    if (!target) {
        return;
    }
    PreviewController &preview = m_services.preview;
    const playback::Player::State state = preview.player().state();
    const bool loaded = preview.player().isActive() || state == playback::Player::State::Ended;
    const QString key = core::previewKeyForResult(target->searchId, target->index);
    if (toggleIfLoaded && loaded && key == m_playingKey) {
        preview.toggle();
        return;
    }
    m_askedPreviewKey = key;
    m_player->showMessage(askingMessage(source, target->peer));
    m_services.engine.previewResult(target->searchId, target->index, source);
}

void MainWindow::previewGetTracksLine(bool toggleIfLoaded, core::PreviewSource source)
{
    const std::optional<GetTracksView::PreviewTarget> target = m_getTracksView->previewTarget();
    if (!target) {
        return;
    }
    PreviewController &preview = m_services.preview;
    const playback::Player::State state = preview.player().state();
    const bool loaded = preview.player().isActive() || state == playback::Player::State::Ended;
    const QString key = core::previewKeyForLine(target->lineId);
    if (toggleIfLoaded && loaded && key == m_playingKey) {
        preview.toggle();
        return;
    }
    m_askedPreviewKey = key;
    m_player->showMessage(askingMessage(source, target->peer));
    m_services.engine.previewTrack(target->lineId, source);
}

void MainWindow::previewInboxSelection(bool toggleIfLoaded)
{
    const std::optional<InboxView::PreviewTarget> target = m_inboxView->previewTarget();
    if (!target) {
        return;
    }
    const LocalPreview local = previewForPath(target->path, target->track);
    if (!local.request) {
        m_player->showMessage(local.error);
        return;
    }
    PreviewController &preview = m_services.preview;
    const playback::Player::State state = preview.player().state();
    const bool loaded = preview.player().isActive() || state == playback::Player::State::Ended;
    if (toggleIfLoaded && loaded && local.request->identity == preview.identity()) {
        preview.toggle();
        return;
    }
    m_askedPreviewKey.clear(); // a peer preview playing gives way
    m_playingKey.clear();
    m_playingIdentity = local.request->identity;
    if (m_previewId != 0) {
        m_services.engine.stopPreview();
        m_previewId = 0;
    }
    preview.preview(*local.request);
    refreshPreviewActions();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveState();
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        m_notifier->userReturned(); // what arrived while the user was away has been seen
    }
}

void MainWindow::showSearchInSidebar(const core::ManualSearch &search)
{
    using State = core::ManualSearch::State;
    const bool busy =
        search.state == State::LookingUp || search.state == State::Waiting || search.state == State::Searching;
    m_navModel->setSearch(search.id, search.text, search.files, busy);
}

bool MainWindow::navigateTo(const QString &id)
{
    if (const QString peer = NavModel::browseOfItem(id); !peer.isEmpty()) {
        if (m_navModel->rowOf(id) < 0) {
            return false; // closed
        }
        m_browseView->showPeer(peer);
        m_stack->setCurrentWidget(m_browseView);
        m_inspector->showPanelFor(u"browse"_s);
        m_currentId = id;
        m_sidebar->select(id);
        refreshTitle();
        Q_EMIT viewChanged(id);
        return true;
    }
    if (const qint64 searchId = NavModel::searchOfItem(id); searchId > 0) {
        if (!m_services.engine.search(searchId)) {
            return false; // closed, or from an earlier run
        }
        m_searchView->showSearch(searchId);
        m_stack->setCurrentWidget(m_searchView);
        m_inspector->showPanelFor(u"search"_s);
        m_currentId = id;
        m_sidebar->select(id);
        refreshTitle();
        Q_EMIT viewChanged(id);
        return true;
    }
    const auto found = m_viewIndex.constFind(id);
    if (found == m_viewIndex.cend()) {
        if (!id.isEmpty()) {
            qCWarning(lcUi) << "unknown view" << id;
        }
        return false;
    }
    m_stack->setCurrentIndex(static_cast<int>(found.value()));
    m_inspector->showPanelFor(id);
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
    for (const View &view : m_views) {
        ids.append(view.id);
    }
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
    if ((m_currentId == u"search"_s || NavModel::searchOfItem(m_currentId) > 0) && m_title) {
        m_title->setText(m_searchView->title());
        m_subtitle->setText(m_searchView->summary());
        return;
    }
    if ((m_currentId == u"browse"_s || !NavModel::browseOfItem(m_currentId).isEmpty()) && m_title) {
        m_title->setText(m_browseView->title());
        m_subtitle->setText(m_browseView->summary());
        return;
    }
    const auto found = m_viewIndex.constFind(m_currentId);
    if (found == m_viewIndex.cend() || !m_title) {
        return;
    }
    const View &view = m_views.at(found.value());
    m_title->setText(m_navModel->titleOf(view.id));
    m_subtitle->setText(view.subtitle ? view.subtitle() : QString());
}

void MainWindow::refreshNetworkState()
{
    const bool paused = m_services.engine.isPaused();
    if (!m_shownPaused || *m_shownPaused != paused) {
        // Only on a change: an action's icon resyncs the native menus, far too slow for every budget tick.
        m_shownPaused = paused;
        const QSignalBlocker blocker(m_togglePause);
        m_togglePause->setChecked(paused);
        m_togglePause->setIcon(paused ? icons::symbol(u"play.circle"_s, u"media-playback-start"_s)
                                      : icons::symbol(u"pause.circle"_s, u"media-playback-pause"_s));
        m_togglePause->setToolTip(paused ? tr("Resume network activity")
                                         : tr("Pause network activity (no new searches or transfers)"));
    }

    const int available = m_services.engine.searchesAvailable();
    m_budget->setText(QString::number(available));
    m_budget->setToolTip(tr("Searches you can run now: %1 of %2, one more every %3 s")
                             .arg(available)
                             .arg(governor::limits::kSearchBurst)
                             .arg(governor::limits::kSearchRefill.count()));

    using State = core::NetworkStatus::State;
    const core::NetworkStatus network = m_services.engine.networkStatus();
    QString status;
    bool online = false;
    switch (network.state) {
    case State::Online:
        status = tr("Online as %1").arg(network.username);
        online = true;
        break;
    case State::Connecting:
        status = tr("Connecting…");
        break;
    case State::Reconnecting:
        status = tr("Reconnecting soon");
        break;
    case State::NeedsUser:
        status = tr("Offline: see Settings");
        break;
    case State::Offline:
        status = tr("Offline");
        break;
    case State::NotConfigured:
        status = tr("Not set up");
        break;
    }
    if (paused) {
        status = online ? tr("Paused · online as %1").arg(network.username) : tr("Network paused");
    }
    m_sidebar->setConnectionStatus(status, online && !paused);
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
    const QList<QAction *> actions = m_appearanceGroup->actions();
    for (QAction *action : actions) {
        const QSignalBlocker blocker(action);
        action->setChecked(action->data().toInt() == static_cast<int>(appearance));
    }
    m_settingsView->setAppearance(appearance);
}

} // namespace selecta::app
