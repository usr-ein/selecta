// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MainWindow.h"

#include "app/AppSettings.h"
#include "core/EngineHost.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QListView>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::Appearance;
using selecta::app::AppSettings;
using selecta::app::MainWindow;

namespace {

class MainWindowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        session = sessions.start(1'000);
        settings = std::make_unique<AppSettings>(dir.filePath(u"settings.ini"_s));
        selecta::core::EngineOptions options;
        options.databasePath = dir.filePath(u"engine.sqlite3"_s);
        engine = std::make_unique<selecta::core::EngineHost>(options); // started only by the tests that need it
    }

    std::unique_ptr<MainWindow> makeWindow()
    {
        selecta::app::BuildInfo build;
        build.version = u"0.0.0-test"_s;
        build.session = session;
        auto window =
            std::make_unique<MainWindow>(selecta::app::AppServices{db, sessions, *settings, *engine, build, 1'000});
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
        return window;
    }

    QTemporaryDir dir;
    selecta::storage::Database db;
    selecta::storage::SessionLog sessions{db};
    int session = 0;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<selecta::core::EngineHost> engine;
};

} // namespace

TEST_F(MainWindowTest, OpensOnGetTracksByDefault)
{
    auto window = makeWindow();
    EXPECT_EQ(window->currentViewId(), u"get"_s);
    EXPECT_EQ(window->currentTitle(), u"Get tracks"_s);
}

TEST_F(MainWindowTest, EverySidebarItemHasAView)
{
    auto window = makeWindow();
    const QStringList ids = window->viewIds();
    for (const QString &id : ids) {
        EXPECT_TRUE(window->navigateTo(id)) << id.toStdString();
        EXPECT_EQ(window->currentViewId(), id);
        EXPECT_FALSE(window->currentTitle().isEmpty()) << id.toStdString();
        const auto *stack = window->findChild<QStackedWidget *>(u"content"_s);
        ASSERT_NE(stack, nullptr);
        ASSERT_NE(stack->currentWidget(), nullptr);
        EXPECT_EQ(stack->currentWidget()->objectName(), u"view."_s + id);
    }
}

TEST_F(MainWindowTest, UnknownViewChangesNothing)
{
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"history"_s));
    const QSignalSpy changed(window.get(), &MainWindow::viewChanged);
    EXPECT_FALSE(window->navigateTo(u"does-not-exist"_s));
    EXPECT_FALSE(window->navigateTo(QString()));
    EXPECT_EQ(window->currentViewId(), u"history"_s);
    EXPECT_EQ(changed.count(), 0);
}

TEST_F(MainWindowTest, SidebarSelectionFollowsNavigation)
{
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"collection"_s));
    const auto *list = window->findChild<QListView *>(u"sidebar.list"_s);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->currentIndex().data().toString(), u"Collection"_s);
}

TEST_F(MainWindowTest, TogglesPanesAndRestoresThem)
{
    {
        auto window = makeWindow();
        auto *toggleSidebar = window->findChild<QAction *>(u"action.toggleSidebar"_s);
        auto *toggleInspector = window->findChild<QAction *>(u"action.toggleInspector"_s);
        ASSERT_NE(toggleSidebar, nullptr);
        ASSERT_NE(toggleInspector, nullptr);
        toggleSidebar->trigger();
        toggleInspector->trigger();
        EXPECT_FALSE(window->isSidebarShown());
        EXPECT_FALSE(window->isInspectorShown());
        EXPECT_EQ(toggleSidebar->text(), u"Show Sidebar"_s);
        ASSERT_TRUE(window->navigateTo(u"settings"_s));
        window->saveState();
    }
    auto window = makeWindow();
    EXPECT_FALSE(window->isSidebarShown());
    EXPECT_FALSE(window->isInspectorShown());
    EXPECT_EQ(window->currentViewId(), u"settings"_s);
}

TEST_F(MainWindowTest, AppearanceIsAppliedEverywhereAndSaved)
{
    {
        auto window = makeWindow();
        window->setAppearance(Appearance::Dark);
        auto *dark = window->findChild<QAction *>(u"action.appearance.dark"_s);
        ASSERT_NE(dark, nullptr);
        EXPECT_TRUE(dark->isChecked());
        const auto *popup = window->findChild<QComboBox *>(u"settings.appearance"_s);
        ASSERT_NE(popup, nullptr);
        EXPECT_EQ(popup->currentData().toInt(), static_cast<int>(Appearance::Dark));
        window->saveState();
    }
    EXPECT_EQ(AppSettings(dir.filePath(u"settings.ini"_s)).appearance(), Appearance::Dark);
    selecta::app::applyAppearance(Appearance::System);
}

TEST_F(MainWindowTest, HistoryListsTheCurrentSession)
{
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"history"_s));
    EXPECT_TRUE(window->currentSubtitle().contains(QString::number(session)));
    const auto *table = window->findChild<QAbstractItemView *>(u"history.sessions"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_TRUE(table->model()->index(0, 0).data().toString().startsWith(QString::number(session)));
}

TEST_F(MainWindowTest, PauseActionDrivesTheEngineAndIsRemembered)
{
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    auto *pause = window->findChild<QAction *>(u"action.togglePause"_s);
    ASSERT_NE(pause, nullptr);
    EXPECT_FALSE(pause->isChecked());

    QSignalSpy paused(engine.get(), &selecta::core::EngineHost::pausedChanged);
    pause->trigger();
    ASSERT_TRUE(paused.wait(5000));
    EXPECT_TRUE(engine->isPaused());
    EXPECT_TRUE(pause->isChecked());
    EXPECT_TRUE(settings->networkPaused());

    ASSERT_TRUE(window->navigateTo(u"activity"_s));
    EXPECT_EQ(window->currentSubtitle(), u"Network paused"_s);
    engine->stop();
}
