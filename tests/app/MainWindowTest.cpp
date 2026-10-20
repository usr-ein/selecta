// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/MainWindow.h"

#include "SyntheticAudio.h"
#include "app/AppSettings.h"
#include "app/Notifications.h"
#include "app/Presets.h"
#include "app/Preview.h"
#include "app/SpectrogramView.h"
#include "app/Toasts.h"
#include "app/devtools/DebugServer.h"
#include "app/models/ConverterModel.h"
#include "app/views/ConverterView.h"
#include "audio/AnalysisCache.h"
#include "convert/TagWriter.h"
#include "core/Albums.h"
#include "core/EngineHost.h"
#include "core/MatchCorrections.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"
#include "support/FakeNotificationSink.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtEndian>

#include <cmath>
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
        options.metaEndpoints = selecta::core::MetaEndpoints::unreachable(); // tests never reach real services
        engine = std::make_unique<selecta::core::EngineHost>(options);       // started only by the tests that need it
        preview = std::make_unique<selecta::app::PreviewController>(selecta::playback::AudioOutput::createSilent(),
                                                                    nullptr); // never the real audio device
    }

    std::unique_ptr<MainWindow> makeWindow()
    {
        selecta::app::BuildInfo build;
        build.version = u"0.0.0-test"_s;
        build.session = session;
        auto window = std::make_unique<MainWindow>(
            selecta::app::AppServices{db, sessions, *settings, *engine, *preview, build, 1'000, &notifications});
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
    std::unique_ptr<selecta::app::PreviewController> preview;
    selecta::testing::FakeNotificationSink notifications; // never the system's notification centre
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
    EXPECT_TRUE(table->model()->index(0, 0).data().toString().startsWith(u"Session %1"_s.arg(session)));
}

TEST_F(MainWindowTest, HistoryShowsDownloadsUnderTheirSession)
{
    selecta::library::HistoryEntry entry;
    entry.session = session;
    entry.completedAt = 2'000;
    entry.peer = u"vinyl_ghost"_s;
    entry.remotePath = QByteArray("@@vinyl\\Okuma Drift - Neon Rain.flac");
    entry.quality = u"FLAC 16/44.1"_s;
    entry.artist = u"Okuma Drift"_s;
    entry.title = u"Neon Rain"_s;
    entry.whereabouts = selecta::library::HistoryEntry::Whereabouts::Trashed;
    ASSERT_TRUE(selecta::library::HistoryStore(db).add(entry).has_value());

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"history"_s));
    const auto *table = window->findChild<QAbstractItemView *>(u"history.sessions"_s);
    ASSERT_NE(table, nullptr);
    const QModelIndex sessionRow = table->model()->index(0, 0);
    EXPECT_TRUE(sessionRow.data().toString().contains(u"1 download"_s));
    ASSERT_EQ(table->model()->rowCount(sessionRow), 1);
    EXPECT_EQ(table->model()->index(0, 0, sessionRow).data().toString(), u"Okuma Drift — Neon Rain"_s);
    EXPECT_EQ(table->model()->index(0, 3, sessionRow).data().toString(), u"Moved to the Trash"_s);
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

TEST_F(MainWindowTest, InboxListsItemsAndFilesTheSelection)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    library.collectionRoots = {dir.filePath(u"Music"_s)};
    ASSERT_TRUE(QDir().mkpath(library.inboxRoot + u"/Singles"_s) && QDir().mkpath(library.collectionRoots.first()));
    {
        QFile song(library.inboxRoot + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s);
        ASSERT_TRUE(song.open(QIODevice::WriteOnly) && song.write("audio") == 5);
    }
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem item;
        item.relativePath = u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
        item.state = selecta::library::InboxItem::State::Ready;
        item.artist = u"Okuma Drift"_s;
        item.title = u"Neon Rain"_s;
        item.format = u"MP3"_s;
        item.bitrate = 320;
        item.createdAt = 1000;
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(item).has_value());
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy changed(engine.get(), &selecta::core::EngineHost::inboxChanged);
    engine->start();
    ASSERT_TRUE(changed.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    EXPECT_EQ(window->currentSubtitle(), u"1 item waiting"_s);
    auto *table = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->model()->rowCount(), 1); // the Singles group
    const QModelIndex group = table->model()->index(0, 0);
    ASSERT_EQ(table->model()->rowCount(group), 1);
    table->selectionModel()->select(table->model()->index(0, 0, group),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    auto *file = window->findChild<QPushButton *>(u"inbox.file"_s);
    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(file->isEnabled());

    QSignalSpy finished(engine.get(), &selecta::core::EngineHost::inboxOperationFinished);
    file->click();
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(library.collectionRoots.first() + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s));
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentSubtitle() == u"Nothing waiting"_s; }, 5000));
    engine->stop();
}

namespace {

// A mono 16-bit PCM WAV of a 440 Hz tone.
QByteArray toneWav(double seconds, int rate = 44'100)
{
    const auto frames = static_cast<quint32>(seconds * rate);
    QByteArray data;
    data.reserve(static_cast<qsizetype>(frames) * 2);
    for (quint32 i = 0; i < frames; ++i) {
        const auto sample = static_cast<qint16>(8000.0 * std::sin(2.0 * M_PI * 440.0 * i / rate));
        const auto bits = static_cast<quint16>(sample);
        data.append(static_cast<char>(bits & 0xFFU));
        data.append(static_cast<char>(bits >> 8U));
    }
    const auto le32 = [](quint32 value) {
        QByteArray bytes(4, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    const auto le16 = [](quint16 value) {
        QByteArray bytes(2, '\0');
        qToLittleEndian(value, bytes.data());
        return bytes;
    };
    const auto size = static_cast<quint32>(data.size());
    return "RIFF" + le32(36 + size) + "WAVEfmt " + le32(16) + le16(1) + le16(1) + le32(rate) + le32(rate * 2) +
           le16(2) + le16(16) + "data" + le32(size) + data;
}

} // namespace

TEST_F(MainWindowTest, InboxItemsPreviewWithSpaceAndDoubleClick)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(library.inboxRoot + u"/Singles"_s));
    {
        QFile song(library.inboxRoot + u"/Singles/Okuma Drift - Neon Rain.wav"_s);
        const QByteArray wav = toneWav(3.0);
        ASSERT_TRUE(song.open(QIODevice::WriteOnly) && song.write(wav) == wav.size());
    }
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem item;
        item.relativePath = u"Singles/Okuma Drift - Neon Rain.wav"_s;
        item.state = selecta::library::InboxItem::State::Ready;
        item.artist = u"Okuma Drift"_s;
        item.title = u"Neon Rain"_s;
        item.format = u"WAV"_s;
        item.createdAt = 1000;
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(item).has_value());
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy changed(engine.get(), &selecta::core::EngineHost::inboxChanged);
    engine->start();
    ASSERT_TRUE(changed.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *table = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    // Waits are qWaitFor() in ASSERT_TRUE: a QTRY_* that fails only returns from the test, a pass for GoogleTest.
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    const QModelIndex row = table->model()->index(0, 0, table->model()->index(0, 0));
    ASSERT_TRUE(row.isValid());
    table->setCurrentIndex(row);
    table->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    using State = selecta::playback::Player::State;
    const selecta::playback::Player &player = preview->player();
    window->playOrPause(); // what Space does
    EXPECT_TRUE(preview->identity().startsWith(u"file|"_s));
    ASSERT_TRUE(QTest::qWaitFor([&] { return player.state() == State::Playing; }, 5000))
        << player.error().toStdString();
    auto *title = window->findChild<QLabel *>(u"player.title"_s);
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), u"Neon Rain"_s);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !preview->waveform().isNull(); }, 5000)); // analysed alongside

    // Discard is for previews the cache holds: a file of the user's stays where it is, and goes on playing.
    auto *discard = window->findChild<QAction *>(u"action.discardPreview"_s);
    ASSERT_NE(discard, nullptr);
    EXPECT_FALSE(discard->isEnabled());
    window->discardPreview(); // what D does
    EXPECT_EQ(player.state(), State::Playing);
    EXPECT_TRUE(preview->identity().startsWith(u"file|"_s));
    EXPECT_TRUE(QFileInfo::exists(library.inboxRoot + u"/Singles/Okuma Drift - Neon Rain.wav"_s));
    const auto *subtitle = window->findChild<QLabel *>(u"player.subtitle"_s);
    ASSERT_NE(subtitle, nullptr);
    EXPECT_TRUE(subtitle->text().startsWith(u"Only previews can be discarded"_s)) // said while it plays too
        << subtitle->text().toStdString();

    window->playOrPause(); // the same item again: pause, not restart
    EXPECT_EQ(player.state(), State::Paused);
    window->playOrPause();
    EXPECT_EQ(player.state(), State::Playing);

    auto *stop = window->findChild<QAction *>(u"action.stopPreview"_s);
    ASSERT_NE(stop, nullptr);
    stop->trigger();
    EXPECT_EQ(player.state(), State::Idle);
    EXPECT_EQ(title->text(), u"Nothing playing"_s);

    // The list is rebuilt whenever the Inbox changes: look the row up again rather than keep an old index.
    const QModelIndex again = table->model()->index(0, 0, table->model()->index(0, 0));
    ASSERT_TRUE(again.isValid());
    // QTest sends a double click as the second press alone: the first click comes before it, as with a mouse.
    const QPoint centre = table->visualRect(again).center();
    QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, centre);
    EXPECT_EQ(player.state(), State::Idle); // a click only selects
    QTest::mouseDClick(table->viewport(), Qt::LeftButton, {}, centre);
    ASSERT_TRUE(QTest::qWaitFor([&] { return player.state() == State::Playing; }, 5000))
        << player.error().toStdString();

    // A file that went missing says so instead of playing.
    preview->stop();
    ASSERT_TRUE(QFile::remove(library.inboxRoot + u"/Singles/Okuma Drift - Neon Rain.wav"_s));
    window->playOrPause();
    EXPECT_EQ(player.state(), State::Idle);
    EXPECT_TRUE(subtitle->text().contains(u"isn't there any more"_s)) << subtitle->text().toStdString();
    engine->stop();
}

namespace {

// The fake yt-dlp from meta's tests, behind a wrapper that names the search it answers; the real one never runs in
// tests (CLAUDE.md).
bool writeFakeYtDlp(const QString &path)
{
    const QString fixtures = QStringLiteral(SELECTA_META_FIXTURES);
    const QByteArray wrapper =
        u"#!/bin/sh\n"
        u"FAKE_YTDLP_SEARCH='%1'\n"
        u"export FAKE_YTDLP_SEARCH\n"
        u"exec /bin/sh '%2' \"$@\"\n"_s
            .arg(fixtures + u"/youtube/search_glass_harbour.json"_s, fixtures + u"/fake-yt-dlp"_s)
            .toUtf8();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wrapper) != wrapper.size()) {
        return false;
    }
    file.close();
    return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

} // namespace

TEST_F(MainWindowTest, DiscardDeletesTheYouTubePreviewFromTheCache)
{
    const QString ytDlp = dir.filePath(u"yt-dlp"_s);
    ASSERT_TRUE(writeFakeYtDlp(ytDlp));
    selecta::core::EngineOptions options;
    options.databasePath = dir.filePath(u"engine.sqlite3"_s);
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    options.metaEndpoints.ytDlp = true; // the fake one
    options.ytDlpPath = ytDlp;
    options.previewDir = dir.filePath(u"Previews"_s);
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"get"_s));
    engine->addTracks(u"Kestrel Moon - Glass Harbour (Extended Mix) 6:31\n"_s);
    auto *table = window->findChild<QTreeView *>(u"get.lines"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    const QModelIndex line = table->model()->index(0, 0);
    table->setCurrentIndex(line);
    table->selectionModel()->select(line, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto *discard = window->findChild<QToolButton *>(u"player.discard"_s);
    ASSERT_NE(discard, nullptr);
    EXPECT_FALSE(discard->isEnabled()); // nothing loaded yet

    // Space hears it from YouTube (D26): the fake's video from the Topic channel, fetched into the preview cache.
    const auto *subtitle = window->findChild<QLabel *>(u"player.subtitle"_s);
    ASSERT_NE(subtitle, nullptr);
    QSignalSpy changes(engine.get(), &selecta::core::EngineHost::previewChanged);
    window->playOrPause();
    ASSERT_TRUE(QTest::qWaitFor([&] { return preview->identity() == u"youtube|KmTopic00Ex"_s; }, 30'000))
        << subtitle->text().toStdString();
    const QString audio = dir.filePath(u"Previews/YouTube/KmTopic00Ex.m4a"_s);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return !changes.isEmpty() && changes.constLast().at(0).value<selecta::core::Preview>().state ==
                                             selecta::core::Preview::State::Complete;
        },
        30'000));
    EXPECT_EQ(QFileInfo(audio).size(), 40'960);
    EXPECT_TRUE(discard->isEnabled());

    QSignalSpy caches(engine.get(), &selecta::core::EngineHost::cachesChanged);
    discard->click();
    EXPECT_EQ(preview->player().state(), selecta::playback::Player::State::Idle);
    EXPECT_TRUE(preview->identity().isEmpty());
    EXPECT_TRUE(subtitle->text().startsWith(u"Discarded"_s)) << subtitle->text().toStdString();
    EXPECT_FALSE(discard->isEnabled());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !caches.isEmpty(); }, 5000));
    EXPECT_EQ(caches.constLast().at(0).value<selecta::core::CacheStatus>().previewBytes, 0);
    EXPECT_FALSE(QFile::exists(audio));
    engine->stop();
}

TEST_F(MainWindowTest, GetTracksListsPastedLinesAndSaysWhatHappened)
{
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"get"_s));
    EXPECT_EQ(window->currentSubtitle(), u"Nothing in the list"_s);

    QSignalSpy added(engine.get(), &selecta::core::EngineHost::tracksAdded);
    engine->addTracks(u"Okuma Drift - Neon Rain\nHalden & Rue - Tidewater (Original Mix)\nID - ID\n"_s);
    ASSERT_TRUE(added.wait(5000));
    EXPECT_EQ(added.at(0).at(0).toInt(), 2);
    EXPECT_EQ(added.at(0).at(2).toInt(), 1);

    auto *table = window->findChild<QTreeView *>(u"get.lines"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 2; }, 5000));
    EXPECT_EQ(table->model()->index(0, 1).data().toString(), u"Okuma Drift - Neon Rain"_s);
    EXPECT_TRUE(window->currentSubtitle().startsWith(u"2 tracks"_s));
    const auto *note = window->findChild<QLabel *>(u"get.note"_s);
    ASSERT_NE(note, nullptr);
    EXPECT_TRUE(note->text().contains(u"Added 2 tracks."_s));
    EXPECT_TRUE(note->text().contains(u"unidentified"_s));

    // Removing a line from the list.
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QSignalSpy reset(engine.get(), &selecta::core::EngineHost::getLinesReset);
    QTest::keyClick(table, Qt::Key_Backspace);
    ASSERT_TRUE(reset.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    engine->stop();
}

TEST_F(MainWindowTest, ASearchOpensInTheSidebarAndClosesFromIt)
{
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    auto *field = window->findChild<QLineEdit *>(u"sidebar.search"_s);
    ASSERT_NE(field, nullptr);
    field->setText(u"Okuma Drift - Neon Rain"_s);
    QTest::keyClick(field, Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentViewId().startsWith(u"search:"_s); }, 5000));
    EXPECT_TRUE(field->text().isEmpty());
    EXPECT_EQ(window->currentTitle(), u"Okuma Drift - Neon Rain"_s);
    const QString id = window->currentViewId();
    auto *list = window->findChild<QListView *>(u"sidebar.list"_s);
    ASSERT_NE(list, nullptr);
    bool listed = false;
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        listed = listed || list->model()->index(row, 0).data(Qt::UserRole + 1).toString() == id;
    }
    EXPECT_TRUE(listed);
    // Offline here: the search waits for the network, and says so.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !window->currentSubtitle().isEmpty(); }, 5000));

    // The same text again opens the same search.
    field->setText(u"okuma drift - neon rain"_s);
    QTest::keyClick(field, Qt::Key_Return);
    QTest::qWait(200);
    EXPECT_EQ(window->currentViewId(), id);

    engine->closeSearch(id.mid(7).toLongLong());
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentViewId() == u"get"_s; }, 5000));
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        EXPECT_NE(list->model()->index(row, 0).data(Qt::UserRole + 1).toString(), id);
    }
    engine->stop();
}

TEST_F(MainWindowTest, TheInboxInspectorShowsAndEditsTags)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(library.inboxRoot + u"/Singles"_s));
    const QString file = library.inboxRoot + u"/Singles/Okuma_Drift_-_Neon_Rain.wav"_s;
    {
        QFile song(file);
        const QByteArray wav = toneWav(1.0);
        ASSERT_TRUE(song.open(QIODevice::WriteOnly) && song.write(wav) == wav.size());
    }
    selecta::convert::MetadataModel tags;
    tags.set(selecta::convert::keys::Artist, u"Okuma Drift"_s, selecta::convert::Provenance::Release);
    tags.set(selecta::convert::keys::Title, u"Neon Rain"_s, selecta::convert::Provenance::Release);
    tags.set(selecta::convert::keys::Label, u"Nightfall Records"_s, selecta::convert::Provenance::Release);
    ASSERT_TRUE(selecta::convert::writeTags(file, tags).ok());
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    qint64 id = 0;
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem item;
        item.relativePath = u"Singles/Okuma_Drift_-_Neon_Rain.wav"_s;
        item.state = selecta::library::InboxItem::State::Ready;
        item.artist = u"Okuma Drift"_s;
        item.title = u"Neon Rain"_s;
        item.format = u"WAV"_s;
        item.tagSource = u"spotify"_s;
        item.peer = u"vinyl_ghost"_s;
        item.sourceFormat = u"FLAC 16/44.1"_s;
        item.createdAt = 1000;
        id = selecta::library::InboxStore(setup).add(item).value_or(0);
        ASSERT_GT(id, 0);
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy changed(engine.get(), &selecta::core::EngineHost::inboxChanged);
    engine->start();
    ASSERT_TRUE(changed.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *table = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    const QModelIndex row = table->model()->index(0, 0, table->model()->index(0, 0));
    table->setCurrentIndex(row);
    table->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    const auto *title = window->findChild<QLabel *>(u"inspector.inbox.title"_s);
    const auto *tagList = window->findChild<QLabel *>(u"inspector.inbox.tags"_s);
    const auto *provenance = window->findChild<QLabel *>(u"inspector.inbox.provenance"_s);
    const auto *source = window->findChild<QLabel *>(u"inspector.inbox.source"_s);
    ASSERT_TRUE(title && tagList && provenance && source);
    EXPECT_EQ(title->text(), u"Neon Rain"_s);
    ASSERT_TRUE(QTest::qWaitFor([&] { return tagList->text().contains(u"Nightfall Records"_s); }, 5000));
    EXPECT_TRUE(provenance->text().contains(u"Spotify"_s));
    EXPECT_TRUE(source->text().contains(u"vinyl_ghost"_s));
    EXPECT_TRUE(source->text().contains(u"FLAC 16/44.1"_s));

    // An edit is written into the file; the list and the inspector follow.
    selecta::convert::MetadataModel edit;
    edit.set(selecta::convert::keys::Title, u"Neon Rain (Dub)"_s, selecta::convert::Provenance::User);
    QSignalSpy finished(engine.get(), &selecta::core::EngineHost::inboxOperationFinished);
    engine->editInboxTags(id, edit, {selecta::convert::keys::Label.toString()});
    ASSERT_TRUE(finished.wait(5000));
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    ASSERT_TRUE(QTest::qWaitFor([&] { return title->text() == u"Neon Rain (Dub)"_s; }, 5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return !tagList->text().contains(u"Nightfall Records"_s) && tagList->text().contains(u"Neon Rain (Dub)"_s);
        },
        5000));
    EXPECT_TRUE(provenance->text().contains(u"edited by you"_s));
    engine->stop();
}

TEST_F(MainWindowTest, TheInboxInspectorShowsAnAlbumsCoverAndSaysNothingUntrue)
{
    // Q10: an album shows its cover.jpg, or else its first track's picture. Q11: no word about Choose Release, which
    // is for singles.
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    const QString withJpg = library.inboxRoot + u"/Albums/Okuma_Drift_-_Night_Signals"_s;
    const QString embedded = library.inboxRoot + u"/Albums/Kassia_Moreau_-_Harbour_Lights"_s;
    ASSERT_TRUE(QDir().mkpath(withJpg) && QDir().mkpath(embedded));
    QImage red(300, 300, QImage::Format_RGB32);
    red.fill(Qt::red);
    ASSERT_TRUE(red.save(withJpg + u"/cover.jpg"_s, "JPG"));
    const QByteArray wav = toneWav(0.5);
    const QStringList tracks{withJpg + u"/01 Neon Rain.wav"_s, embedded + u"/01 Harbour Lights.wav"_s,
                             embedded + u"/02 Low Tide.wav"_s};
    for (const QString &track : tracks) {
        QFile file(track);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size());
    }
    {
        QByteArray png;
        QBuffer buffer(&png);
        ASSERT_TRUE(buffer.open(QIODevice::WriteOnly) && red.save(&buffer, "PNG"));
        selecta::convert::MetadataModel tags;
        tags.set(selecta::convert::keys::Title, u"Harbour Lights"_s, selecta::convert::Provenance::Release);
        selecta::convert::Picture cover;
        cover.data = png;
        cover.mimeType = u"image/png"_s;
        tags.setFrontCover(cover);
        ASSERT_TRUE(selecta::convert::writeTags(embedded + u"/01 Harbour Lights.wav"_s, tags).ok());
    }
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem album;
        album.kind = selecta::library::InboxItem::Kind::Album;
        album.relativePath = u"Albums/Okuma_Drift_-_Night_Signals"_s;
        album.state = selecta::library::InboxItem::State::Ready;
        album.artist = u"Okuma Drift"_s;
        album.album = u"Night Signals"_s;
        album.format = u"WAV"_s;
        album.createdAt = 2000;
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(album).has_value());
        album.relativePath = u"Albums/Kassia_Moreau_-_Harbour_Lights"_s;
        album.artist = u"Kassia Moreau"_s;
        album.album = u"Harbour Lights"_s;
        album.createdAt = 1000;
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(album).has_value());
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy changed(engine.get(), &selecta::core::EngineHost::inboxChanged);
    engine->start();
    ASSERT_TRUE(changed.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *table = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    const QModelIndex albums = table->model()->index(0, 0);
    ASSERT_EQ(table->model()->rowCount(albums), 2);
    const auto *cover = window->findChild<QLabel *>(u"inspector.inbox.cover"_s);
    const auto *title = window->findChild<QLabel *>(u"inspector.inbox.title"_s);
    const auto *provenance = window->findChild<QLabel *>(u"inspector.inbox.provenance"_s);
    ASSERT_TRUE(cover && title && provenance);
    for (int row = 0; row < 2; ++row) {
        const QModelIndex index = table->model()->index(row, 0, albums);
        table->setCurrentIndex(index);
        table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        EXPECT_EQ(title->text(), row == 0 ? u"Night Signals"_s : u"Harbour Lights"_s);
        // QTest::qWaitFor, not QTRY_…: a QTRY timeout returns from the test without failing it under gtest.
        EXPECT_TRUE(QTest::qWaitFor([cover] { return !cover->pixmap().isNull(); }, 5000)) << row;
        EXPECT_EQ(cover->accessibleName(), u"Cover"_s);
        EXPECT_FALSE(provenance->text().contains(u"Choose Release"_s)) << provenance->text().toStdString();
        EXPECT_TRUE(provenance->text().contains(u"No release matched the album"_s));
    }
    engine->stop();
}

TEST_F(MainWindowTest, AnAlbumThatReplacedASingleSaysSoWithUndo)
{
    auto window = makeWindow();
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    selecta::core::AlbumDownload album;
    album.id = 4;
    album.folder = "Music\\Okuma Drift - Night Signals (2024)";
    album.state = selecta::core::AlbumDownload::State::Arrived;
    Q_EMIT engine->albumChanged(album);
    EXPECT_EQ(toasts->count(), 0); // its report tells it
    selecta::core::AlbumReport report;
    report.albumId = 4;
    report.album = u"Night Signals"_s;
    report.arrival = true;
    report.complete = true;
    report.tracks = 9;
    report.where = u"Albums/Okuma_Drift_-_Night_Signals_(2024)"_s;
    report.trashed = {u"Okuma Drift – Neon Rain"_s};
    Q_EMIT engine->albumReported(report);

    // One notification for both, with Undo.
    ASSERT_EQ(toasts->count(), 1);
    const QString text = toasts->texts().constFirst();
    EXPECT_TRUE(text.startsWith(
        u"“Night Signals” is in your Inbox\n9 tracks in Inbox/Albums/Okuma_Drift_-_Night_Signals_(2024)/\n"_s))
        << text.toStdString();
    EXPECT_TRUE(text.contains(u"“Okuma Drift – Neon Rain” was in your Inbox as a single"_s)) << text.toStdString();
    auto *undo = toasts->findChild<QPushButton *>(u"toast.1.action.0"_s);
    ASSERT_NE(undo, nullptr);
    EXPECT_EQ(undo->text(), u"Undo"_s);
    undo->click(); // the engine isn't running here: only the notification goes
    EXPECT_EQ(toasts->count(), 0);
}

TEST_F(MainWindowTest, ManySinglesOnAnAlbumAreAskedAbout)
{
    auto window = makeWindow();
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    selecta::core::AlbumReport report;
    report.albumId = 8;
    report.album = u"Orchard Orchard"_s;
    report.arrival = true;
    report.complete = true;
    report.held = {u"A – One"_s, u"A – Two"_s, u"A – Three"_s, u"A – Four"_s};
    Q_EMIT engine->albumReported(report);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_TRUE(toasts->texts().constFirst().contains(u"4 singles in your Inbox are on it"_s));
    auto *move = toasts->findChild<QPushButton *>(u"toast.1.action.0"_s);
    ASSERT_NE(move, nullptr);
    EXPECT_EQ(move->text(), u"Move to Trash"_s);

    // Once moved, the news comes with Undo.
    report.arrival = false;
    report.trashed = report.held;
    report.held.clear();
    Q_EMIT engine->albumReported(report);
    ASSERT_EQ(toasts->count(), 2);
    EXPECT_TRUE(
        toasts->texts().constFirst().startsWith(u"Moved 4 singles to the Trash\nThey're on “Orchard Orchard”"_s))
        << toasts->texts().constFirst().toStdString();
}

TEST_F(MainWindowTest, AnAlbumArrivesFailsOrComesIncomplete)
{
    auto window = makeWindow();
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    selecta::core::AlbumReport report;
    report.albumId = 5;
    report.album = u"Harbour Lights"_s;
    report.arrival = true;
    report.tracks = 7;
    report.where = u"Albums/Kassia_Moreau_-_Harbour_Lights"_s;
    report.warning = u"1 file of the album couldn't be downloaded or converted."_s;
    Q_EMIT engine->albumReported(report);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_EQ(toasts->texts().constFirst(),
              u"“Harbour Lights” arrived incomplete\n7 tracks in Inbox/Albums/Kassia_Moreau_-_Harbour_Lights/\n"_s +
                  report.warning);

    selecta::core::AlbumDownload album;
    album.id = 6;
    album.folder = "Music\\Kassia Moreau - Harbour Lights";
    album.state = selecta::core::AlbumDownload::State::Failed;
    album.message = u"The folder holds no audio file."_s;
    Q_EMIT engine->albumChanged(album);
    ASSERT_EQ(toasts->count(), 2);
    EXPECT_EQ(toasts->texts().constFirst(), u"Couldn't download “Kassia Moreau - Harbour Lights”\n"_s + album.message);

    // Downloading isn't news.
    album.id = 7;
    album.state = selecta::core::AlbumDownload::State::Downloading;
    Q_EMIT engine->albumChanged(album);
    EXPECT_EQ(toasts->count(), 2);
}

TEST_F(MainWindowTest, ItemsMovedToTheTrashTogetherAreOneNotificationWithUndo)
{
    auto window = makeWindow();
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    for (const QString &name : {u"Singles/One.mp3"_s, u"Singles/Two.mp3"_s}) {
        selecta::core::TrashedItem item;
        item.item.relativePath = name;
        item.trashedPath = dir.filePath(u"Trash/"_s + QFileInfo(name).fileName());
        Q_EMIT engine->inboxItemTrashed(item);
    }
    ASSERT_TRUE(QTest::qWaitFor([&] { return toasts->count() == 1; }, 2000));
    EXPECT_EQ(toasts->texts().constFirst(), u"Moved 2 items to the Trash"_s);
    EXPECT_NE(toasts->findChild<QPushButton *>(u"toast.1.action.0"_s), nullptr);
}

TEST_F(MainWindowTest, TheConverterMakesANewFileNextToTheOriginal)
{
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"converter"_s));
    auto *view = window->findChild<selecta::app::ConverterView *>(u"view.converter"_s);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(window->currentSubtitle(), u"Drop files to convert"_s);

    // A WAV in a folder named like an album: converted to MP3 320, tagged from the names.
    const QString folder = dir.filePath(u"Okuma Drift - Night Signals (2024)"_s);
    ASSERT_TRUE(QDir().mkpath(folder));
    QFile wav(folder + u"/01 - Neon Rain.wav"_s);
    const QByteArray bytes = toneWav(2.0);
    ASSERT_TRUE(wav.open(QIODevice::WriteOnly) && wav.write(bytes) == bytes.size());
    wav.close();
    view->addPaths({folder});
    selecta::app::ConverterModel *model = view->model();
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return model->rowCount() == 1 &&
                   model->items().constFirst().state == selecta::core::ConverterItem::State::Ready;
        },
        10'000));
    EXPECT_EQ(window->currentSubtitle(), u"1 file → MP3 320 kbps CBR"_s);
    EXPECT_EQ(model->items().constFirst().tags.value(u"TITLE"_s), u"Neon Rain"_s);

    auto *convert = window->findChild<QPushButton *>(u"converter.convert"_s);
    ASSERT_NE(convert, nullptr);
    ASSERT_TRUE(convert->isEnabled());
    convert->click();
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return model->items().constFirst().state == selecta::core::ConverterItem::State::Done; }, 30'000))
        << model->items().constFirst().message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(folder + u"/01 - Neon Rain.mp3"_s));
    EXPECT_TRUE(QFileInfo::exists(folder + u"/01 - Neon Rain.wav"_s)); // the original stays
    auto *status = window->findChild<QLabel *>(u"converter.status"_s);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->text(), u"1 file converted."_s);
    engine->stop();
}

TEST_F(MainWindowTest, SinglesAlsoOnAnAlbumAreFlaggedInTheInboxAndTheCollection)
{
    auto window = makeWindow();
    // An Inbox single the album holds: its status says so.
    selecta::core::InboxEntry entry;
    entry.item.id = 7;
    entry.item.kind = selecta::library::InboxItem::Kind::Single;
    entry.item.relativePath = u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
    entry.item.state = selecta::library::InboxItem::State::Ready;
    entry.item.artist = u"Okuma Drift"_s;
    entry.item.title = u"Neon Rain"_s;
    entry.alsoOn = {u"Night Signals"_s};
    Q_EMIT engine->inboxChanged({entry});
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *items = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(items, nullptr);
    const QModelIndex singles = items->model()->index(0, 0);
    ASSERT_EQ(items->model()->rowCount(singles), 1);
    EXPECT_EQ(items->model()->index(0, 4, singles).data().toString(), u"⚑ Also on “Night Signals”"_s);

    // A filed single: listed in the Collection view, which never touches it.
    selecta::core::SupersedeFlag flag;
    flag.id = 3;
    flag.album = u"Night Signals"_s;
    flag.singlePath = u"/Music/Singles/Okuma Drift - Neon Rain.mp3"_s;
    flag.single = u"Okuma Drift – Neon Rain"_s;
    Q_EMIT engine->supersedeFlagsChanged({flag});
    auto *flags = window->findChild<QListView *>(u"collection.flags"_s);
    ASSERT_NE(flags, nullptr);
    ASSERT_EQ(flags->model()->rowCount(), 1);
    EXPECT_EQ(flags->model()->index(0, 0).data().toString(), u"⚑ Okuma Drift – Neon Rain is also on “Night Signals”"_s);
}

TEST_F(MainWindowTest, SearchesTypedAreOfferedAgain)
{
    auto window = makeWindow();
    auto *field = window->findChild<QLineEdit *>(u"sidebar.search"_s);
    ASSERT_NE(field, nullptr);
    for (const QString &text : {u"Okuma Drift - Neon Rain"_s, u"Kassia Moreau"_s, u"okuma drift - neon rain"_s}) {
        field->setText(text);
        QTest::keyClick(field, Qt::Key_Return);
    }
    // Newest first, the same search once (SRCH-15).
    EXPECT_EQ(settings->recentSearches(), (QStringList{u"okuma drift - neon rain"_s, u"Kassia Moreau"_s}));
    ASSERT_NE(field->completer(), nullptr);
    EXPECT_EQ(field->completer()->model()->rowCount(), 2);
}

TEST_F(MainWindowTest, TheWishlistListsSavedSearchesAndSaysWhenOneFindsSomething)
{
    auto window = makeWindow();
    selecta::core::WishlistItem item;
    item.id = 3;
    item.text = u"Okuma Drift Neon Rain"_s;
    item.lastRunMs = QDateTime::currentMSecsSinceEpoch() - 120'000;
    item.found = 4;
    Q_EMIT engine->wishlistChanged({item});
    ASSERT_TRUE(window->navigateTo(u"wishlist"_s));
    EXPECT_EQ(window->currentSubtitle(), u"1 saved search · 4 new files"_s);
    auto *items = window->findChild<QTreeView *>(u"wishlist.items"_s);
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->model()->rowCount(), 1);
    EXPECT_EQ(items->model()->index(0, 0).data().toString(), u"Okuma Drift Neon Rain"_s);
    EXPECT_EQ(items->model()->index(0, 1).data().toString(), u"Any audio file"_s); // saved before presets
    EXPECT_EQ(items->model()->index(0, 2).data().toString(), u"2 min ago"_s);
    EXPECT_EQ(items->model()->index(0, 3).data().toString(), u"4 new"_s);
    // The sidebar shows how many new files wait.
    auto *list = window->findChild<QListView *>(u"sidebar.list"_s);
    ASSERT_NE(list, nullptr);
    int count = 0;
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        if (list->model()->index(row, 0).data(Qt::UserRole + 1).toString() == u"wishlist"_s) {
            count = list->model()->index(row, 0).data(Qt::UserRole + 3).toInt();
        }
    }
    EXPECT_EQ(count, 4);

    // A background run found something: a notification with Show.
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    Q_EMIT engine->wishlistFound(item, 2);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_EQ(toasts->texts().constFirst(), u"Wishlist: 2 new files for “Okuma Drift Neon Rain”"_s);
    EXPECT_NE(toasts->findChild<QPushButton *>(u"toast.1.action.0"_s), nullptr);
}

TEST_F(MainWindowTest, MessagesShowConversationsWhatIsUnreadAndWhatArrives)
{
    auto window = makeWindow();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    Q_EMIT engine->conversationsChanged({selecta::core::Conversation{.peer = u"lossless_lena"_s,
                                                                     .lastText = u"Yes, 24/96 from the LP"_s,
                                                                     .lastAtMs = now - 120'000,
                                                                     .lastOutgoing = false,
                                                                     .unread = 2}});
    ASSERT_TRUE(window->navigateTo(u"messages"_s));
    EXPECT_EQ(window->currentSubtitle(), u"1 conversation · 2 unread"_s);
    auto *list = window->findChild<QTreeView *>(u"messages.conversations"_s);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 1);
    EXPECT_EQ(list->model()->index(0, 0).data().toString(), u"lossless_lena\nYes, 24/96 from the LP"_s);
    EXPECT_EQ(list->model()->index(0, 1).data().toString(), u"2 min ago\n2 new"_s);
    // The sidebar counts what is unread (NET-7).
    auto *sidebar = window->findChild<QListView *>(u"sidebar.list"_s);
    ASSERT_NE(sidebar, nullptr);
    int count = 0;
    for (int row = 0; row < sidebar->model()->rowCount(); ++row) {
        if (sidebar->model()->index(row, 0).data(Qt::UserRole + 1).toString() == u"messages"_s) {
            count = sidebar->model()->index(row, 0).data(Qt::UserRole + 3).toInt();
        }
    }
    EXPECT_EQ(count, 2);

    // A message from someone else: a notification, whose Reply opens the conversation.
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    const selecta::core::ChatMessage message{
        .id = 7, .peer = u"vinyl_ghost"_s, .atMs = now, .outgoing = false, .text = u"Got the B-side?"_s, .read = false};
    Q_EMIT engine->messageAdded(message);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_EQ(toasts->texts().constFirst(), u"Message from vinyl_ghost\nGot the B-side?"_s);
    auto *reply = toasts->findChild<QPushButton *>(u"toast.1.action.0"_s);
    ASSERT_NE(reply, nullptr);
    reply->click();
    auto *peer = window->findChild<QLabel *>(u"messages.peer"_s);
    ASSERT_NE(peer, nullptr);
    EXPECT_EQ(peer->text(), u"vinyl_ghost"_s);
    // Until it has messages, a new conversation shows at the top.
    ASSERT_EQ(list->model()->rowCount(), 2);
    EXPECT_EQ(list->model()->index(0, 0).data().toString(), u"vinyl_ghost\nNew conversation"_s);
    Q_EMIT engine->conversationLoaded(u"vinyl_ghost"_s, {message});
    auto *thread = window->findChild<QTextBrowser *>(u"messages.thread"_s);
    ASSERT_NE(thread, nullptr);
    EXPECT_TRUE(thread->toPlainText().contains(u"Got the B-side?"_s));

    // People: the lists, with buddies' status (NET-8).
    Q_EMIT engine->userListsChanged(
        {selecta::core::ListedUser{.username = u"leech"_s, .list = selecta::core::UserList::Banned},
         selecta::core::ListedUser{.username = u"kassia"_s,
                                   .list = selecta::core::UserList::Buddy,
                                   .presence = selecta::core::Presence::Online}});
    auto *people = window->findChild<QTreeView *>(u"people.table"_s);
    ASSERT_NE(people, nullptr);
    ASSERT_EQ(people->model()->rowCount(), 2);
    EXPECT_EQ(people->model()->index(0, 0).data().toString(), u"kassia"_s);
    EXPECT_EQ(people->model()->index(0, 1).data().toString(), u"Buddy"_s);
    EXPECT_EQ(people->model()->index(0, 2).data().toString(), u"Online"_s);
    EXPECT_EQ(people->model()->index(1, 1).data().toString(), u"Banned"_s);
}

TEST_F(MainWindowTest, RoomsShowWhatYoureInWhatIsUnreadAndWhoNamesYou)
{
    auto window = makeWindow();
    Q_EMIT engine->roomListChanged({selecta::core::RoomListing{.name = u"dnb"_s, .users = 40},
                                    selecta::core::RoomListing{.name = u"nicotine"_s, .users = 120}});
    Q_EMIT engine->roomChanged(selecta::core::Room{.name = u"dnb"_s,
                                                   .state = selecta::core::Room::State::Joined,
                                                   .members = {u"lossless_lena"_s, u"selecta_test"_s},
                                                   .unread = 3,
                                                   .mentions = 1,
                                                   .message = {}});
    ASSERT_TRUE(window->navigateTo(u"rooms"_s));
    EXPECT_EQ(window->currentSubtitle(), u"In 1 room · 3 unread"_s);
    auto *list = window->findChild<QTreeView *>(u"rooms.list"_s);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 2);
    EXPECT_EQ(list->model()->index(0, 0).data().toString(), u"nicotine"_s); // the busiest first
    auto *joined = window->findChild<QTreeView *>(u"rooms.joined"_s);
    ASSERT_NE(joined, nullptr);
    ASSERT_EQ(joined->model()->rowCount(), 1);
    EXPECT_EQ(joined->model()->index(0, 1).data().toString(), u"@ 3"_s);
    auto *sidebar = window->findChild<QListView *>(u"sidebar.list"_s);
    ASSERT_NE(sidebar, nullptr);
    int count = 0;
    for (int row = 0; row < sidebar->model()->rowCount(); ++row) {
        if (sidebar->model()->index(row, 0).data(Qt::UserRole + 1).toString() == u"rooms"_s) {
            count = sidebar->model()->index(row, 0).data(Qt::UserRole + 3).toInt();
        }
    }
    EXPECT_EQ(count, 3);

    // Opened, the room shows its members and the lines that come.
    joined->setCurrentIndex(joined->model()->index(0, 0));
    auto *name = window->findChild<QLabel *>(u"rooms.name"_s);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), u"dnb"_s);
    EXPECT_EQ(window->findChild<QLabel *>(u"rooms.details"_s)->text(), u"2 members"_s);
    EXPECT_EQ(window->findChild<QListView *>(u"rooms.members"_s)->model()->rowCount(), 2);
    Q_EMIT engine->roomLineAdded(u"dnb"_s, selecta::core::RoomLine{.atMs = QDateTime::currentMSecsSinceEpoch(),
                                                                   .user = u"lossless_lena"_s,
                                                                   .text = u"selecta_test: got the B-side?"_s,
                                                                   .own = false,
                                                                   .mention = true});
    auto *lines = window->findChild<QTextBrowser *>(u"rooms.lines"_s);
    ASSERT_NE(lines, nullptr);
    EXPECT_TRUE(lines->toPlainText().contains(u"got the B-side?"_s));
    // The window isn't active in the tests, so being named brings a notification.
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_EQ(toasts->texts().constFirst(), u"lossless_lena mentioned you in dnb\nselecta_test: got the B-side?"_s);

    // A name nobody uses would create a room: Selecta asks first.
    auto *filter = window->findChild<QLineEdit *>(u"rooms.filter"_s);
    ASSERT_NE(filter, nullptr);
    filter->setText(u"deep house"_s);
    QTest::keyClick(filter, Qt::Key_Return);
    auto *confirm = window->findChild<QMessageBox *>(u"rooms.createConfirm"_s);
    ASSERT_NE(confirm, nullptr);
    EXPECT_EQ(confirm->text(), u"Create the room “deep house”?"_s);
    confirm->reject();
}

TEST_F(MainWindowTest, JoiningARoomOpensIt)
{
    auto window = makeWindow();
    Q_EMIT engine->roomListChanged({selecta::core::RoomListing{.name = u"dnb"_s, .users = 40}});
    ASSERT_TRUE(window->navigateTo(u"rooms"_s));
    auto *list = window->findChild<QTreeView *>(u"rooms.list"_s);
    ASSERT_NE(list, nullptr);
    list->setCurrentIndex(list->model()->index(0, 0));
    auto *join = window->findChild<QPushButton *>(u"rooms.join"_s);
    ASSERT_NE(join, nullptr);
    join->click();

    // The engine has the room, still joining: it opens without another click (QA Q16).
    Q_EMIT engine->roomChanged(selecta::core::Room{.name = u"dnb"_s,
                                                   .state = selecta::core::Room::State::Joining,
                                                   .members = {},
                                                   .unread = 0,
                                                   .mentions = 0,
                                                   .message = {}});
    auto *name = window->findChild<QLabel *>(u"rooms.name"_s);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), u"dnb"_s);
    auto *joined = window->findChild<QTreeView *>(u"rooms.joined"_s);
    ASSERT_NE(joined, nullptr);
    EXPECT_EQ(joined->currentIndex().data().toString(), u"dnb"_s);

    // A room joined at login, or by another window, doesn't take over the view.
    Q_EMIT engine->roomChanged(selecta::core::Room{.name = u"house"_s,
                                                   .state = selecta::core::Room::State::Joined,
                                                   .members = {},
                                                   .unread = 0,
                                                   .mentions = 0,
                                                   .message = {}});
    EXPECT_EQ(name->text(), u"dnb"_s);
}

TEST_F(MainWindowTest, NewsWhileAwayReachesTheSystemAndAClickOpensIt)
{
    auto window = makeWindow();
    selecta::app::Notifier *notifier = window->notifier();
    ASSERT_NE(notifier, nullptr);
    EXPECT_EQ(notifier->sink(), &notifications);
    ASSERT_TRUE(notifier->isAway()); // the window isn't active in the tests
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);

    // A message: the window's notification, and the system's.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const selecta::core::ChatMessage message{
        .id = 7, .peer = u"vinyl_ghost"_s, .atMs = now, .outgoing = false, .text = u"Got the B-side?"_s, .read = false};
    Q_EMIT engine->messageAdded(message);
    EXPECT_EQ(toasts->count(), 1);
    ASSERT_EQ(notifications.posted.size(), 1);
    EXPECT_EQ(notifications.posted.constFirst().title, u"Message from vinyl_ghost"_s);
    EXPECT_EQ(notifications.posted.constFirst().body, u"Got the B-side?"_s);

    // The Dock badge: unread messages, and what arrived while away.
    Q_EMIT engine->conversationsChanged({selecta::core::Conversation{
        .peer = u"vinyl_ghost"_s, .lastText = message.text, .lastAtMs = now, .lastOutgoing = false, .unread = 1}});
    EXPECT_EQ(notifier->badge(), 1);
    selecta::core::AlbumReport report;
    report.albumId = 4;
    report.album = u"Night Signals"_s;
    report.arrival = true;
    report.complete = true;
    report.tracks = 9;
    report.where = u"Albums/Okuma_Drift_-_Night_Signals_(2024)"_s;
    Q_EMIT engine->albumReported(report);
    EXPECT_EQ(notifier->badge(), 2);
    ASSERT_EQ(notifications.posted.size(), 2);
    EXPECT_EQ(notifications.posted.constLast().title, u"“Night Signals” is in your Inbox"_s);

    // Named in a room: a click brings the room up.
    Q_EMIT engine->roomChanged(selecta::core::Room{.name = u"dnb"_s,
                                                   .state = selecta::core::Room::State::Joined,
                                                   .members = {u"lossless_lena"_s, u"selecta_test"_s},
                                                   .unread = 0,
                                                   .mentions = 0,
                                                   .message = {}});
    Q_EMIT engine->roomLineAdded(u"dnb"_s, selecta::core::RoomLine{.atMs = now,
                                                                   .user = u"lossless_lena"_s,
                                                                   .text = u"selecta_test: got the B-side?"_s,
                                                                   .own = false,
                                                                   .mention = true});
    ASSERT_EQ(notifications.posted.size(), 3);
    EXPECT_EQ(notifications.posted.constLast().target, (selecta::app::NotificationTarget{u"rooms"_s, u"dnb"_s}));
    notifications.click(notifications.posted.constLast().target);
    EXPECT_EQ(window->currentViewId(), u"rooms"_s);
    EXPECT_EQ(window->findChild<QLabel *>(u"rooms.name"_s)->text(), u"dnb"_s);

    // A click on the message opens the conversation; on the album, the Inbox.
    notifications.click(notifications.posted.constFirst().target);
    EXPECT_EQ(window->currentViewId(), u"messages"_s);
    EXPECT_EQ(window->findChild<QLabel *>(u"messages.peer"_s)->text(), u"vinyl_ghost"_s);
    notifications.click(notifications.posted.at(1).target);
    EXPECT_EQ(window->currentViewId(), u"inbox"_s);
    // Something that isn't a view changes nothing.
    notifications.click({u"no-such-view"_s, {}});
    EXPECT_EQ(window->currentViewId(), u"inbox"_s);
}

TEST_F(MainWindowTest, AFinishedGetTracksBatchSaysSoInTheWindowAndToTheSystem)
{
    auto window = makeWindow();
    auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    selecta::core::GetLine line;
    line.id = 3;
    line.artist = u"Okuma Drift"_s;
    line.title = u"Neon Rain"_s;
    line.raw = u"Okuma Drift - Neon Rain"_s;
    line.downloadState = selecta::core::GetLine::DownloadState::Queued;
    Q_EMIT engine->getLinesReset({line});
    line.downloadState = selecta::core::GetLine::DownloadState::Converting;
    Q_EMIT engine->getLineChanged(line);
    EXPECT_EQ(toasts->count(), 0);
    EXPECT_TRUE(notifications.posted.isEmpty());

    line.downloadState = selecta::core::GetLine::DownloadState::Arrived;
    Q_EMIT engine->getLineChanged(line);
    ASSERT_EQ(toasts->count(), 1);
    EXPECT_EQ(toasts->texts().constFirst(), u"Get tracks finished\n“Okuma Drift – Neon Rain” is in your Inbox."_s);
    ASSERT_EQ(notifications.posted.size(), 1);
    EXPECT_EQ(notifications.posted.constFirst().target, (selecta::app::NotificationTarget{u"inbox"_s, {}}));
    EXPECT_EQ(window->notifier()->badge(), 1);
    auto *show = toasts->findChild<QPushButton *>(u"toast.1.action.0"_s);
    ASSERT_NE(show, nullptr);
    EXPECT_EQ(show->text(), u"Show"_s);
    show->click();
    EXPECT_EQ(window->currentViewId(), u"inbox"_s);
}

TEST_F(MainWindowTest, ShowNotificationsCanBeTurnedOffInSettings)
{
    {
        auto window = makeWindow();
        EXPECT_TRUE(window->notifier()->isEnabled()); // on by default
        ASSERT_TRUE(window->navigateTo(u"settings"_s));
        auto *box = window->findChild<QCheckBox *>(u"settings.showNotifications"_s);
        ASSERT_NE(box, nullptr);
        EXPECT_TRUE(box->isChecked());
        box->click();
        EXPECT_FALSE(window->notifier()->isEnabled());
        EXPECT_FALSE(settings->showNotifications());

        Q_EMIT engine->messageAdded(selecta::core::ChatMessage{
            .id = 1, .peer = u"vinyl_ghost"_s, .atMs = 0, .outgoing = false, .text = u"hi"_s, .read = false});
        EXPECT_TRUE(notifications.posted.isEmpty());
        auto *toasts = window->findChild<selecta::app::Toasts *>(u"toasts"_s);
        ASSERT_NE(toasts, nullptr);
        EXPECT_EQ(toasts->count(), 1); // the window still says it
        window->saveState();
    }
    auto window = makeWindow();
    EXPECT_FALSE(window->notifier()->isEnabled());
    auto *box = window->findChild<QCheckBox *>(u"settings.showNotifications"_s);
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(box->isChecked());
}

TEST_F(MainWindowTest, AClickBringsTheWindowForwardUnlessItIsKeptOffTheScreen)
{
    auto window = makeWindow();
    // The test platform makes each new window the active one: this one takes over, as if the user were elsewhere.
    QWidget elsewhere;
    elsewhere.setAttribute(Qt::WA_DontShowOnScreen);
    elsewhere.show();
    ASSERT_TRUE(QTest::qWaitFor([&] { return elsewhere.isActiveWindow(); }, 5000));
    ASSERT_TRUE(window->notifier()->isAway());
    selecta::core::AlbumReport report;
    report.albumId = 4;
    report.album = u"Night Signals"_s;
    report.arrival = true;
    report.complete = true;
    report.tracks = 9;
    report.where = u"Albums/Okuma_Drift_-_Night_Signals_(2024)"_s;
    Q_EMIT engine->albumReported(report);
    ASSERT_EQ(notifications.posted.size(), 1);
    ASSERT_EQ(window->notifier()->arrivalsWhileAway(), 1);

    // Kept off the screen (headless runs): the click shows what it was about and takes no focus, which would leave
    // the keyboard with a window nobody can see.
    notifications.click(notifications.posted.constFirst().target);
    EXPECT_EQ(window->currentViewId(), u"inbox"_s);
    EXPECT_FALSE(QTest::qWaitFor([&] { return window->isActiveWindow(); }, 200));
    EXPECT_EQ(window->notifier()->arrivalsWhileAway(), 1);

    // On the screen, the window comes forward: the user is back, and what arrived while away has been seen.
    window->hide();
    window->setAttribute(Qt::WA_DontShowOnScreen, false);
    window->show();
    QCoreApplication::processEvents(); // whatever showing it again asked for
    elsewhere.activateWindow();
    ASSERT_TRUE(QTest::qWaitFor([&] { return elsewhere.isActiveWindow(); }, 5000));
    ASSERT_TRUE(window->notifier()->isAway());
    ASSERT_TRUE(window->navigateTo(u"get"_s));
    notifications.click(notifications.posted.constFirst().target);
    EXPECT_EQ(window->currentViewId(), u"inbox"_s);
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->isActiveWindow(); }, 5000));
    EXPECT_FALSE(window->notifier()->isAway());
    EXPECT_EQ(window->notifier()->arrivalsWhileAway(), 0);
    EXPECT_EQ(window->notifier()->badge(), 0);
}

TEST_F(MainWindowTest, PresetsAreSharedBySearchGetTracksAndTheWishlist)
{
    auto window = makeWindow();
    auto *presets = window->findChild<selecta::app::Presets *>();
    ASSERT_NE(presets, nullptr);
    auto *searchPresets = window->findChild<QComboBox *>(u"search.preset"_s);
    auto *getPresets = window->findChild<QComboBox *>(u"get.preset"_s);
    ASSERT_NE(searchPresets, nullptr);
    ASSERT_NE(getPresets, nullptr);
    EXPECT_EQ(searchPresets->count(), 4);
    EXPECT_EQ(getPresets->count(), 4);
    EXPECT_EQ(getPresets->currentData().toString(), selecta::core::kMp3OrLosslessPreset);

    // A preset saved once shows everywhere (SRCH-8).
    selecta::core::SearchFilter dj;
    dj.setAllowed(selecta::core::SearchFormat::Mp3, true);
    dj.bitrateRule = selecta::core::SearchFilter::BitrateRule::Equal;
    dj.bitrateKbps = 320;
    const std::optional<QString> id = presets->save(u"DJ — MP3 320"_s, dj);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(searchPresets->count(), 5);
    ASSERT_EQ(getPresets->count(), 5);
    EXPECT_EQ(getPresets->itemText(4), u"DJ — MP3 320"_s);

    // Get tracks' choice is remembered; deleting the preset falls back to the default.
    getPresets->setCurrentIndex(4);
    Q_EMIT getPresets->activated(4);
    EXPECT_EQ(settings->getTracksPresetId(), *id);
    presets->remove(*id);
    EXPECT_EQ(getPresets->count(), 4);
    EXPECT_EQ(getPresets->currentData().toString(), selecta::core::kMp3OrLosslessPreset);
}

// PREV-9: an item whose spectrum check found something shows a badge in the list and the inspector, and the
// inspector draws its spectrogram from the analysis cache with the cut-off marked.
TEST_F(MainWindowTest, TheInboxShowsTheSpectrumCheck)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(library.inboxRoot + u"/Singles"_s));
    for (const QString &name : {u"Suspect.wav"_s, u"Genuine.wav"_s}) {
        QFile song(library.inboxRoot + u"/Singles/"_s + name);
        const QByteArray wav = toneWav(1.0);
        ASSERT_TRUE(song.open(QIODevice::WriteOnly) && song.write(wav) == wav.size());
    }
    settings->setLibraryConfig(library);

    // The arrival's report, cached as the arrival keeps it: likely from a 128 kbps MP3, cut at 16.7 kHz.
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    QByteArray levels(qsizetype{100} * 64, '\0');
    for (int c = 0; c < 100; ++c) {
        for (int b = 0; b < 48; ++b) { // up to about 16.5 kHz
            levels[(qsizetype{c} * 64) + b] = static_cast<char>(180);
        }
    }
    selecta::audio::QualityReport report;
    report.sampleRate = 44100;
    report.cutoffHz = 16742;
    report.likelySourceKbps = 128;
    report.sourceBitDepth = 16;
    report.effectiveBitDepth = 16;
    report.findings = {selecta::audio::QualityFinding::LikelyFromLossy};
    report.spectrogram = selecta::audio::Spectrogram::fromLevels(44100, 441000, 100, 64, levels).value();
    const selecta::audio::CacheKey key = selecta::audio::CacheKey::forName(u"vinyl_ghost|Suspect.flac|1"_s);
    ASSERT_TRUE(cache->storeQuality(key, report).ok());
    preview = std::make_unique<selecta::app::PreviewController>(selecta::playback::AudioOutput::createSilent(), cache);

    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem suspect;
        suspect.relativePath = u"Singles/Suspect.wav"_s;
        suspect.state = selecta::library::InboxItem::State::Ready;
        suspect.artist = u"Okuma Drift"_s;
        suspect.title = u"Neon Rain"_s;
        suspect.format = u"WAV"_s;
        suspect.sourceFormat = u"FLAC 16/44.1"_s;
        suspect.createdAt = 2000;
        suspect.qualityChecked = true;
        suspect.qualityFindings = {u"from-lossy"_s};
        suspect.qualityCutoffHz = 16742;
        suspect.qualitySourceKbps = 128;
        suspect.qualityKey = key.toHex();
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(suspect).has_value());
        selecta::library::InboxItem genuine = suspect;
        genuine.relativePath = u"Singles/Genuine.wav"_s;
        genuine.title = u"Halden"_s;
        genuine.createdAt = 1000;
        genuine.qualityFindings.clear();
        genuine.qualityCutoffHz = 21900;
        genuine.qualitySourceKbps.reset();
        genuine.qualityKey = QString(64, u'0'); // not in the cache (evicted, emptied)
        ASSERT_TRUE(selecta::library::InboxStore(setup).add(genuine).has_value());
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy changed(engine.get(), &selecta::core::EngineHost::inboxChanged);
    engine->start();
    ASSERT_TRUE(changed.wait(5000));

    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *table = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 1; }, 5000));
    const QModelIndex singles = table->model()->index(0, 0);
    ASSERT_EQ(table->model()->rowCount(singles), 2);
    // Newest first: the suspect file, with its badge, a symbol and words (UI-4); the genuine one is just ready.
    const QModelIndex suspectStatus = table->model()->index(0, 4, singles);
    EXPECT_EQ(suspectStatus.data().toString(), u"⚠ Likely from a ≈ 128 kbps MP3"_s);
    EXPECT_TRUE(suspectStatus.data(Qt::ToolTipRole).toString().contains(u"the spectrum stops at 16.7 kHz"_s))
        << suspectStatus.data(Qt::ToolTipRole).toString().toStdString();
    EXPECT_EQ(table->model()->index(1, 4, singles).data().toString(), u"Ready"_s);
    // VoiceOver hears the symbols in words, and the tooltip as the description (UI-5).
    EXPECT_EQ(suspectStatus.data(Qt::AccessibleTextRole).toString(), u"Warning: Likely from a roughly 128 kbps MP3"_s);
    EXPECT_EQ(suspectStatus.data(Qt::AccessibleDescriptionRole), suspectStatus.data(Qt::ToolTipRole));
    EXPECT_EQ(table->model()->index(1, 4, singles).data(Qt::AccessibleTextRole).toString(), u"Ready"_s);
    EXPECT_EQ(singles.data(Qt::AccessibleTextRole).toString(), u"Singles, 2"_s);

    const QModelIndex row = table->model()->index(0, 0, singles);
    table->setCurrentIndex(row);
    table->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const auto *alsoOn = window->findChild<QLabel *>(u"inspector.inbox.alsoOn"_s);
    const auto *spectrum = window->findChild<QWidget *>(u"inspector.inbox.spectrum"_s);
    const auto *text = window->findChild<QLabel *>(u"inspector.inbox.spectrumText"_s);
    const auto *note = window->findChild<QLabel *>(u"inspector.inbox.spectrumNote"_s);
    const auto *view = window->findChild<selecta::app::SpectrogramView *>(u"inspector.inbox.spectrogram"_s);
    ASSERT_TRUE(alsoOn && spectrum && text && note && view);
    EXPECT_TRUE(alsoOn->text().startsWith(u"⚠ Likely from a ≈ 128 kbps MP3."_s)) << alsoOn->text().toStdString();
    EXPECT_FALSE(spectrum->isHidden());
    EXPECT_FALSE(note->isHidden());
    // The cached report: describeFinding()'s words, the grid and the cut-off.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !view->spectrogram().isNull(); }, 5000));
    EXPECT_EQ(view->spectrogram().columns(), 100);
    EXPECT_EQ(view->cutoff(), 16742);
    EXPECT_TRUE(text->text().startsWith(u"⚠ Likely from a lossy source (≈ 128 kbps MP3"_s))
        << text->text().toStdString();
    EXPECT_FALSE(text->text().contains(u"fake"_s, Qt::CaseInsensitive));

    // The genuine one: checked, nothing found, and its spectrogram is gone from the cache.
    const QModelIndex other = table->model()->index(1, 0, singles);
    table->setCurrentIndex(other);
    table->selectionModel()->select(other, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    ASSERT_TRUE(QTest::qWaitFor([&] { return text->text().startsWith(u"✓ The spectrum reaches 21.9 kHz"_s); }, 5000));
    EXPECT_TRUE(note->isHidden());
    EXPECT_FALSE(alsoOn->text().contains(u"Likely"_s));
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return view->message() == u"The spectrogram is no longer in the cache."_s; }, 5000));
    EXPECT_TRUE(view->spectrogram().isNull());
    engine->stop();
}

// CACHE-2: the analysis cache's line says what it holds and the most it keeps: AnalysisCache::budgetBytes(), its three
// tiers (waveform detail, overviews, spectrum checks).
TEST_F(MainWindowTest, SettingsShowTheAnalysisCacheBudget)
{
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    preview = std::make_unique<selecta::app::PreviewController>(selecta::playback::AudioOutput::createSilent(), cache);
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"settings"_s));
    const auto *line = window->findChild<QLabel *>(u"settings.cache.waveforms"_s);
    ASSERT_NE(line, nullptr);
    EXPECT_EQ(cache->budgetBytes(), qint64{384} * 1024 * 1024);
    ASSERT_TRUE(QTest::qWaitFor([&] { return line->text() == u"0 B, at most 402.7 MB"_s; }, 5000));
    engine->stop();
}

namespace {

// Opens a view's context menu as a right-click at `position` does, and returns its actions' texts; triggers the one
// called `choose`, if any, as the menu closes.
QStringList contextMenu(QWidget *view, QPoint position, const QString &choose = {})
{
    QStringList texts;
    QTimer::singleShot(0, view, [&texts, &choose] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        QAction *chosen = nullptr;
        const QList<QAction *> actions = menu->actions();
        for (QAction *action : actions) {
            if (!action->isSeparator()) {
                texts.append(action->text());
            }
            if (!choose.isEmpty() && action->text() == choose) {
                chosen = action;
            }
        }
        menu->close();
        if (chosen != nullptr) {
            chosen->trigger();
        }
    });
    Q_EMIT view->customContextMenuRequested(position);
    return texts;
}

// The row of an item view whose first column reads `text` (searched depth first).
QModelIndex rowWithText(const QAbstractItemModel &model, const QString &text, const QModelIndex &parent = {})
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data().toString().contains(text)) {
            return index;
        }
        if (const QModelIndex child = rowWithText(model, text, index); child.isValid()) {
            return child;
        }
    }
    return {};
}

// Selects `row` and opens the context menu over it.
QStringList contextMenuOn(QTreeView *view, const QModelIndex &row, const QString &choose = {})
{
    view->setCurrentIndex(row);
    view->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    return contextMenu(view, view->visualRect(row).center(), choose);
}

} // namespace

// LIB-6: a result matched with an owned track can be set apart from it, and a match Selecta isn't sure of confirmed.
TEST_F(MainWindowTest, OwnedSearchResultsOfferCorrections)
{
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    auto *field = window->findChild<QLineEdit *>(u"sidebar.search"_s);
    ASSERT_NE(field, nullptr);
    field->setText(u"Okuma Drift"_s);
    QTest::keyClick(field, Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentViewId().startsWith(u"search:"_s); }, 5000));
    const qint64 searchId = window->currentViewId().mid(7).toLongLong();

    using selecta::core::Ownership;
    const auto result = [](int index, const QString &name, Ownership ownership, quint64 ownedId,
                           selecta::match::Tier tier) {
        selecta::core::SearchResult r;
        r.index = index;
        r.hit.peer = u"vinyl_ghost"_s;
        r.hit.path = (u"@@vinyl\\Music\\"_s + name).toUtf8();
        r.hit.extension = u"flac"_s;
        r.hit.size = 40'000'000;
        r.hit.attributes.duration = 372;
        r.path = selecta::core::decodeRemotePath(r.hit.path);
        r.folder = selecta::core::remoteFolderOf(r.path);
        r.name = selecta::core::remoteNameOf(r.path);
        r.parsed = selecta::match::RemotePathParser::parse(r.path);
        r.ownership = ownership;
        r.ownedId = ownedId;
        if (ownedId != 0) {
            r.candidates = {{ownedId, selecta::match::Source::Collection, tier, selecta::match::Relation::Same}};
        }
        return r;
    };
    Q_EMIT engine->searchResultsReset(
        searchId,
        {result(0, u"Okuma Drift - Neon Rain.flac"_s, Ownership::InCollection, 5, selecta::match::Tier::Exact),
         result(1, u"Okuma Drift - Low Tide.flac"_s, Ownership::LikelyInCollection, 6, selecta::match::Tier::Likely),
         result(2, u"Okuma Drift - Night Signals.flac"_s, Ownership::None, 0, selecta::match::Tier::None)});
    auto *table = window->findChild<QTreeView *>(u"search.results"_s);
    ASSERT_NE(table, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(rowWithText(*table->model(), u"Night Signals"_s).isValid(), 5000);

    const QStringList sure = contextMenuOn(table, rowWithText(*table->model(), u"Neon Rain"_s));
    EXPECT_TRUE(sure.contains(u"Not What I Have"_s)) << sure.join(u", "_s).toStdString();
    EXPECT_FALSE(sure.contains(u"Same as What I Have"_s));
    const QStringList likely = contextMenuOn(table, rowWithText(*table->model(), u"Low Tide"_s));
    EXPECT_TRUE(likely.contains(u"Not What I Have"_s));
    EXPECT_TRUE(likely.contains(u"Same as What I Have"_s));
    const QStringList none = contextMenuOn(table, rowWithText(*table->model(), u"Night Signals"_s));
    EXPECT_FALSE(none.isEmpty());
    EXPECT_FALSE(none.contains(u"Not What I Have"_s));
    EXPECT_FALSE(none.contains(u"Same as What I Have"_s));
    engine->stop();
}

// LIB-6: "Not What I Have, Find It" on a line found in the collection is remembered, and the line is looked for.
TEST_F(MainWindowTest, NotWhatIHaveFindsAGetTracksLine)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    library.collectionRoots = {dir.filePath(u"Music"_s)};
    const QString owned = dir.filePath(u"Music/Singles/Okuma Drift - Neon Rain.mp3"_s);
    ASSERT_TRUE(QDir().mkpath(QFileInfo(owned).absolutePath()) && QDir().mkpath(library.inboxRoot));
    {
        QFile file(owned);
        const QByteArray mp3 = selecta::testing::makeMp3(selecta::testing::sine(440.0, 4.0));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly) && file.write(mp3) == mp3.size());
    }
    selecta::convert::MetadataModel tags;
    tags.set(selecta::convert::keys::Artist, u"Okuma Drift"_s, selecta::convert::Provenance::Embedded);
    tags.set(selecta::convert::keys::Title, u"Neon Rain"_s, selecta::convert::Provenance::Embedded);
    ASSERT_TRUE(selecta::convert::writeTags(owned, tags).ok());
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    engine->addTracks(u"Okuma Drift - Neon Rain"_s);
    using FindState = selecta::core::GetLine::FindState;
    const auto lineState = [this] {
        const QList<selecta::core::GetLine> lines = engine->getLines();
        return lines.isEmpty() ? FindState::Idle : lines.constFirst().findState;
    };
    QTRY_COMPARE_WITH_TIMEOUT(lineState(), FindState::Owned, 10000); // after the collection scan
    auto *table = window->findChild<QTreeView *>(u"get.lines"_s);
    ASSERT_NE(table, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 5000);

    const QStringList texts = contextMenuOn(table, table->model()->index(0, 1), u"Not What I Have, Find It"_s);
    EXPECT_TRUE(texts.contains(u"Not What I Have, Find It"_s)) << texts.join(u", "_s).toStdString();
    QTRY_VERIFY_WITH_TIMEOUT(lineState() != FindState::Owned, 5000);
    engine->stop();

    selecta::storage::Database check;
    ASSERT_TRUE(check.open(engineDb));
    const QList<selecta::core::MatchCorrection> corrections = selecta::core::MatchCorrectionStore(check).all();
    ASSERT_EQ(corrections.size(), 1);
    EXPECT_EQ(corrections.constFirst().kind, selecta::core::CorrectionKind::GetLine);
    EXPECT_EQ(corrections.constFirst().verdict, selecta::core::Verdict::Different);
}

// LIB-6: "Not the Same Track" on a supersede flag, from the Collection view and from the Inbox: the flag goes, and the
// pair is remembered.
TEST_F(MainWindowTest, NotTheSameTrackDropsAFlagForGood)
{
    selecta::core::LibraryConfig library;
    library.inboxRoot = dir.filePath(u"Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(library.inboxRoot + u"/Singles"_s));
    {
        QFile single(library.inboxRoot + u"/Singles/Okuma_Drift_-_Low_Tide.mp3"_s);
        ASSERT_TRUE(single.open(QIODevice::WriteOnly) && single.write("single") == 6);
    }
    settings->setLibraryConfig(library);
    const QString engineDb = dir.filePath(u"engine.sqlite3"_s);
    {
        selecta::storage::Database setup;
        ASSERT_TRUE(setup.open(engineDb));
        ASSERT_TRUE(selecta::storage::migrate(setup, selecta::storage::selectaMigrations()).ok());
        selecta::library::InboxItem item;
        item.relativePath = u"Singles/Okuma_Drift_-_Low_Tide.mp3"_s;
        item.state = selecta::library::InboxItem::State::Ready;
        item.artist = u"Okuma Drift"_s;
        item.title = u"Low Tide"_s;
        item.createdAt = 1000;
        const std::optional<qint64> single = selecta::library::InboxStore(setup).add(item);
        ASSERT_TRUE(single.has_value());
        selecta::core::SupersedeFlagStore flags(setup);
        selecta::core::SupersedeFlag inInbox;
        inInbox.album = u"Night Signals"_s;
        inInbox.singleItem = single;
        inInbox.single = u"Okuma Drift – Low Tide"_s;
        inInbox.createdAt = 1000;
        inInbox.singleTrack = selecta::core::trackIdentity(u"Okuma Drift"_s, u"Low Tide"_s, 4000);
        inInbox.albumTrack = selecta::core::trackIdentity(u"Okuma Drift"_s, u"Low Tide (Album Edit)"_s, 4000);
        ASSERT_TRUE(flags.add(inInbox).has_value());
        selecta::core::SupersedeFlag filed;
        filed.album = u"Night Signals"_s;
        filed.singlePath = u"/Music/Singles/Okuma Drift - Neon Rain.mp3"_s;
        filed.single = u"Okuma Drift – Neon Rain"_s;
        filed.createdAt = 2000;
        filed.singleTrack = selecta::core::trackIdentity(u"Okuma Drift"_s, u"Neon Rain"_s, 3000);
        filed.albumTrack = selecta::core::trackIdentity(u"Okuma Drift"_s, u"Neon Rain"_s, 3000);
        ASSERT_TRUE(flags.add(filed).has_value());
    }
    selecta::core::EngineOptions options;
    options.databasePath = engineDb;
    options.library = library;
    options.metaEndpoints = selecta::core::MetaEndpoints::unreachable();
    engine = std::make_unique<selecta::core::EngineHost>(options);
    QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();

    // The filed single, in the Collection view.
    ASSERT_TRUE(window->navigateTo(u"collection"_s));
    auto *flagList = window->findChild<QListView *>(u"collection.flags"_s);
    ASSERT_NE(flagList, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(flagList->model()->rowCount(), 1, 5000);
    const QStringList texts =
        contextMenu(flagList, flagList->visualRect(flagList->model()->index(0, 0)).center(), u"Not the Same Track"_s);
    EXPECT_TRUE(texts.contains(u"Not the Same Track"_s)) << texts.join(u", "_s).toStdString();
    EXPECT_TRUE(texts.contains(u"Dismiss"_s));
    QTRY_COMPARE_WITH_TIMEOUT(flagList->model()->rowCount(), 0, 5000);

    // The Inbox single: its status said "also on"; the action is in its menu.
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *items = window->findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(items, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(rowWithText(*items->model(), u"Low Tide"_s).isValid(), 5000);
    const QModelIndex single = rowWithText(*items->model(), u"Low Tide"_s);
    EXPECT_EQ(single.siblingAtColumn(4).data().toString(), u"⚑ Also on “Night Signals”"_s);
    const QStringList inboxTexts = contextMenuOn(items, single, u"Not the Same Track"_s);
    EXPECT_TRUE(inboxTexts.contains(u"Not the Same Track"_s)) << inboxTexts.join(u", "_s).toStdString();
    QTRY_COMPARE_WITH_TIMEOUT(rowWithText(*items->model(), u"Low Tide"_s).siblingAtColumn(4).data().toString(),
                              u"Ready"_s, 5000);
    engine->stop();

    selecta::storage::Database check;
    ASSERT_TRUE(check.open(engineDb));
    const QList<selecta::core::MatchCorrection> corrections = selecta::core::MatchCorrectionStore(check).all();
    ASSERT_EQ(corrections.size(), 2);
    for (const selecta::core::MatchCorrection &correction : corrections) {
        EXPECT_EQ(correction.kind, selecta::core::CorrectionKind::Supersede);
        EXPECT_EQ(correction.verdict, selecta::core::Verdict::Different);
    }
    EXPECT_TRUE(selecta::core::SupersedeFlagStore(check).all().isEmpty());
}

#ifdef SELECTA_DEVTOOLS
TEST_F(MainWindowTest, TheDebugSocketSetsSpinBoxesAndPicksComboItems)
{
    // Live test 2026-10-20: `type` couldn't set the Soulseek port, nor `select` pick the preview source.
    auto window = makeWindow();
    selecta::app::devtools::DebugServer server(*window, dir.path());
    const auto send = [&server](const QString &command, const QJsonObject &args) {
        return server.handle(QJsonObject{{"cmd"_L1, command}, {"args"_L1, args}});
    };
    ASSERT_TRUE(send(u"navigate"_s, {{"view"_L1, u"settings"_s}}).value("ok"_L1).toBool());

    auto *port = window->findChild<QSpinBox *>(u"settings.network.port"_s);
    ASSERT_NE(port, nullptr);
    QJsonObject reply = send(u"type"_s, {{"name"_L1, port->objectName()}, {"text"_L1, 2416}}); // a JSON number
    EXPECT_TRUE(reply.value("ok"_L1).toBool()) << reply.value("error"_L1).toString().toStdString();
    EXPECT_EQ(port->value(), 2416);
    EXPECT_TRUE(send(u"type"_s, {{"name"_L1, port->objectName()}, {"text"_L1, u" 2417 "_s}}).value("ok"_L1).toBool());
    EXPECT_EQ(port->value(), 2417);
    for (const QString &bad : {u"70000"_s, u"0"_s, u"twenty"_s, u"24.5"_s}) {
        reply = send(u"type"_s, {{"name"_L1, port->objectName()}, {"text"_L1, bad}});
        EXPECT_FALSE(reply.value("ok"_L1).toBool()) << bad.toStdString();
        EXPECT_TRUE(reply.value("error"_L1).toString().contains(u"1 to 65535"_s));
    }
    EXPECT_EQ(port->value(), 2417);

    auto *source = window->findChild<QComboBox *>(u"settings.preview.source"_s);
    ASSERT_NE(source, nullptr);
    reply = send(u"select"_s, {{"name"_L1, source->objectName()}, {"row"_L1, 2}});
    EXPECT_TRUE(reply.value("ok"_L1).toBool()) << reply.value("error"_L1).toString().toStdString();
    EXPECT_EQ(source->currentIndex(), 2);
    EXPECT_EQ(settings->previewSource(), selecta::core::PreviewSource::Clip); // saved, as when the user picks it
    reply = send(u"select"_s, {{"name"_L1, source->objectName()}, {"text"_L1, u"youtube only"_s}}); // any case
    EXPECT_EQ(reply.value("result"_L1).toString(), u"YouTube only"_s);
    EXPECT_EQ(settings->previewSource(), selecta::core::PreviewSource::YouTube);
    reply = send(u"select"_s, {{"name"_L1, source->objectName()}, {"text"_L1, u"Vinyl"_s}});
    EXPECT_FALSE(reply.value("ok"_L1).toBool());
    EXPECT_TRUE(reply.value("error"_L1).toString().contains(u"YouTube only"_s)); // the items it has
    EXPECT_FALSE(send(u"select"_s, {{"name"_L1, source->objectName()}, {"row"_L1, 9}}).value("ok"_L1).toBool());
    EXPECT_EQ(source->currentIndex(), 1);
}
#endif
