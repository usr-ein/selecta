// SPDX-License-Identifier: GPL-3.0-or-later
// Text from the network or from files is shown as it is (P-019): a peer, a file, a folder or a room named
// "<b>x</b>&y %1" reads "<b>x</b>&y %1" in every label, tooltip and menu, not "x" in bold, "xy" with a shortcut, or a
// name whose "%1" was filled in.
#include "app/AppSettings.h"
#include "app/Format.h"
#include "app/MainWindow.h"
#include "app/NavModel.h"
#include "app/PeerMenu.h"
#include "app/Preview.h"
#include "app/Toasts.h"
#include "app/models/ActivityModel.h"
#include "app/models/ConverterModel.h"
#include "app/models/GetTracksModel.h"
#include "app/models/PeersModel.h"
#include "app/models/SearchResultsModel.h"
#include "app/models/TransfersModel.h"
#include "app/views/EditTagsDialog.h"
#include "app/views/ReleasePickerDialog.h"
#include "convert/MetadataModel.h"
#include "core/EngineHost.h"
#include "library/HistoryStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"
#include "support/FakeNotificationSink.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::MainWindow;
namespace core = selecta::core;
namespace format = selecta::app::format;

namespace {

// Markup, a would-be shortcut and an arg() marker, all in one name.
const QString kName = u"<b>x</b>&y %1"_s;

// What Qt shows of `text` in this format: AutoText is rich text when it looks like markup, as Qt decides.
QString rendered(const QString &text, Qt::TextFormat textFormat)
{
    if (textFormat == Qt::PlainText || (textFormat == Qt::AutoText && !Qt::mightBeRichText(text))) {
        return text;
    }
    QTextDocument document;
    document.setHtml(text);
    return document.toPlainText();
}

QString shown(const QLabel *label)
{
    return label == nullptr ? QString() : rendered(label->text(), label->textFormat());
}

// What a tooltip shows: QToolTip takes text that looks like markup for rich text.
QString shownToolTip(const QVariant &toolTip)
{
    return rendered(toolTip.toString(), Qt::AutoText);
}

// What a button or a menu item shows: "&&" is an "&", and an "&" before a letter makes it a shortcut and goes.
QString shownMenuText(const QString &text)
{
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == u'&' && i + 1 < text.size()) {
            ++i;
        }
        out += text.at(i);
    }
    return out;
}

template <typename T>
T *child(const QObject &root, const QString &name)
{
    T *found = root.findChild<T *>(name);
    EXPECT_NE(found, nullptr) << name.toStdString();
    return found;
}

// Every label under `root` that names kName, in any form, shows it as it is, with no buddy to make a shortcut of
// its "&". `atLeast`: how many such labels there are.
void expectLabelsShowTheName(const QWidget &root, int atLeast)
{
    int seen = 0;
    const QList<QLabel *> labels = root.findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        if (!label->text().contains(u"x</b>"_s) && !label->text().contains(u"x&lt;/b&gt;"_s)) {
            continue;
        }
        ++seen;
        EXPECT_TRUE(shown(label).contains(kName))
            << label->objectName().toStdString() << ": " << label->text().toStdString();
        EXPECT_EQ(label->buddy(), nullptr) << label->objectName().toStdString();
    }
    EXPECT_GE(seen, atLeast);
}

// The menu open now, if any.
QMenu *openMenu()
{
    if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        return menu;
    }
    const QList<QWidget *> windows = QApplication::topLevelWidgets();
    for (QWidget *widget : windows) {
        if (auto *menu = qobject_cast<QMenu *>(widget); menu != nullptr && menu->isVisible()) {
            return menu;
        }
    }
    return nullptr;
}

// The menu a view opens on a right-click at `position`: what its items show. The menu is closed as soon as it is up,
// and the item that shows `choose`, if any, triggered. A menu not found within 5 s fails the test, and whatever popup
// is up then is closed, so a menu that opens unseen can't hang it.
QStringList contextMenu(QWidget *view, QPoint position, const QString &choose = {})
{
    QStringList texts;
    QAction *chosen = nullptr;
    QTimer poll;
    int tries = 0;
    QObject::connect(&poll, &QTimer::timeout, view, [&] {
        QMenu *menu = openMenu();
        if (menu == nullptr) {
            if (++tries == 500) {
                ADD_FAILURE() << "no menu found in 5 s";
                if (QWidget *popup = QApplication::activePopupWidget()) {
                    popup->close();
                }
            }
            return;
        }
        poll.stop();
        const QList<QAction *> actions = menu->actions();
        for (QAction *action : actions) {
            if (action->isSeparator()) {
                continue;
            }
            texts.append(shownMenuText(action->text()));
            if (!choose.isEmpty() && shownMenuText(action->text()) == choose) {
                chosen = action;
            }
        }
        menu->close();
        if (chosen != nullptr) {
            chosen->trigger(); // while the menu, and so the action, still exists
        }
    });
    poll.start(10);
    Q_EMIT view->customContextMenuRequested(position);
    poll.stop();
    return texts;
}

QStringList contextMenuOn(QAbstractItemView *view, const QModelIndex &row)
{
    view->setCurrentIndex(row);
    view->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    return contextMenu(view, view->visualRect(row).center());
}

// The message box open now whose text starts so: a box on its way out (deleted on close) is passed over.
QMessageBox *openBox(const QWidget &root, const QString &textStart)
{
    const QList<QMessageBox *> boxes = root.findChildren<QMessageBox *>();
    for (QMessageBox *box : boxes) {
        if (box->isVisible() && box->text().startsWith(textStart)) {
            return box;
        }
    }
    ADD_FAILURE() << "no box saying " << textStart.toStdString();
    return nullptr;
}

// What a message box shows: its text and its informative text, in its format.
QString shown(const QMessageBox *box)
{
    return box == nullptr
               ? QString()
               : rendered(box->text(), box->textFormat()) + u'\n' + rendered(box->informativeText(), box->textFormat());
}

// The peer menu names the peer as it is.
void expectPeerMenu(const QStringList &texts)
{
    EXPECT_TRUE(texts.contains(u"Browse %1's Files"_s.arg(kName))) << texts.join(u" | "_s).toStdString();
    EXPECT_TRUE(texts.contains(u"Add %1 to Buddies"_s.arg(kName))) << texts.join(u" | "_s).toStdString();
}

// Every tooltip of `model` that names kName, in any form, shows it as it is. Returns how many there are.
int expectToolTipsShowTheName(const QAbstractItemModel &model, const QModelIndex &parent = {})
{
    int seen = 0;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        for (int column = 0; column < model.columnCount(parent); ++column) {
            const QString tip = model.index(row, column, parent).data(Qt::ToolTipRole).toString();
            if (tip.contains(u"x</b>"_s) || tip.contains(u"x&lt;/b&gt;"_s)) {
                ++seen;
                EXPECT_TRUE(shownToolTip(tip).contains(kName)) << tip.toStdString();
            }
        }
        seen += expectToolTipsShowTheName(model, model.index(row, 0, parent));
    }
    return seen;
}

class NetworkTextTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        session = sessions.start(1'000);
        settings =
            std::make_unique<selecta::app::AppSettings>(dir.filePath(u"settings.ini"_s), dir.filePath(u"Inbox"_s));
        core::EngineOptions options;
        options.databasePath = dir.filePath(u"engine.sqlite3"_s);
        options.metaEndpoints = core::MetaEndpoints::unreachable(); // tests never reach real services
        engine = std::make_unique<core::EngineHost>(options);       // started only by the tests that need it
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
    std::unique_ptr<selecta::app::AppSettings> settings;
    std::unique_ptr<core::EngineHost> engine;
    std::unique_ptr<selecta::app::PreviewController> preview;
    selecta::testing::FakeNotificationSink notifications; // never the system's notification centre
};

} // namespace

TEST(NetworkTextFormat, APlainToolTipShowsItsTextAsItIs)
{
    EXPECT_EQ(shownToolTip(format::plainToolTip(kName)), kName);
    EXPECT_EQ(shownToolTip(format::plainToolTip(u"&lt;b&gt; is no tag"_s)), u"&lt;b&gt; is no tag"_s);
    // Lines stay lines.
    EXPECT_EQ(shownToolTip(format::plainToolTip(kName + u"\nsecond line"_s)), kName + u"\nsecond line"_s);
    // Text Qt shows as it is anyway is kept: it looks the same as before.
    for (const QString &plain :
         {u"vinyl_ghost"_s, u"Drum & Bass"_s, u"@@x\\Music\\01 Neon Rain.flac"_s, u"a < b\nsecond line"_s, QString()}) {
        EXPECT_EQ(format::plainToolTip(plain), plain);
    }
}

TEST(NetworkTextFormat, NoMnemonicKeepsEveryAmpersand)
{
    EXPECT_EQ(shownMenuText(format::noMnemonic(kName)), kName);
    EXPECT_EQ(format::noMnemonic(u"Drum & Bass && more"_s), u"Drum && Bass &&&& more"_s);
    EXPECT_EQ(format::noMnemonic(u"vinyl_ghost"_s), u"vinyl_ghost"_s);
}

TEST_F(NetworkTextTest, PeerMenuNamesThePeerAsItIs)
{
    QMenu menu;
    selecta::app::PeerMenu::addActions(menu, kName, *engine,
                                       {.browse = [](const QString &) {}, .message = [](const QString &) {}});
    QStringList texts;
    const QList<QAction *> actions = menu.actions();
    for (const QAction *action : actions) {
        if (!action->isSeparator()) {
            texts.append(shownMenuText(action->text()));
        }
    }
    EXPECT_EQ(texts, (QStringList{u"Browse %1's Files"_s.arg(kName), u"Message %1…"_s.arg(kName),
                                  u"Add %1 to Buddies"_s.arg(kName), u"Ignore %1's Messages"_s.arg(kName),
                                  u"Ban %1 from Your Shares"_s.arg(kName)}));
}

TEST_F(NetworkTextTest, Rooms)
{
    auto window = makeWindow();
    Q_EMIT engine->roomListChanged({core::RoomListing{.name = kName, .users = 40}});
    Q_EMIT engine->roomChanged(core::Room{.name = kName,
                                          .state = core::Room::State::Failed,
                                          .members = {},
                                          .unread = 0,
                                          .mentions = 0,
                                          .message = kName});
    ASSERT_TRUE(window->navigateTo(u"rooms"_s));
    auto *joined = child<QTreeView>(*window, u"rooms.joined"_s);
    ASSERT_NE(joined, nullptr);
    ASSERT_EQ(joined->model()->rowCount(), 1);
    EXPECT_EQ(shownToolTip(joined->model()->index(0, 0).data(Qt::ToolTipRole)), kName); // why it failed
    joined->setCurrentIndex(joined->model()->index(0, 0));
    EXPECT_EQ(shown(child<QLabel>(*window, u"rooms.name"_s)), kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"rooms.details"_s)), kName);

    Q_EMIT engine->roomRefused(kName, kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"rooms.joinProblem"_s)), u"Couldn't join “%1”: %2"_s.arg(kName, kName));
    Q_EMIT engine->roomSaid(kName, kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"rooms.problem"_s)), kName);
    expectLabelsShowTheName(*window, 4);

    // Its members: the peer menu.
    Q_EMIT engine->roomChanged(core::Room{.name = kName,
                                          .state = core::Room::State::Joined,
                                          .members = {kName},
                                          .unread = 0,
                                          .mentions = 0,
                                          .message = {}});
    auto *members = child<QListView>(*window, u"rooms.members"_s);
    ASSERT_NE(members, nullptr);
    ASSERT_EQ(members->model()->rowCount(), 1);
    expectPeerMenu(contextMenuOn(members, members->model()->index(0, 0)));

    // A room nobody uses yet, named as typed.
    const QString fresh = u"<i>new</i>&room %1"_s;
    auto *filter = child<QLineEdit>(*window, u"rooms.filter"_s);
    filter->setText(fresh);
    QTest::keyClick(filter, Qt::Key_Return);
    auto *confirm = child<QMessageBox>(*window, u"rooms.createConfirm"_s);
    ASSERT_NE(confirm, nullptr);
    EXPECT_EQ(rendered(confirm->text(), confirm->textFormat()), u"Create the room “%1”?"_s.arg(fresh));
    confirm->reject();
}

TEST_F(NetworkTextTest, Messages)
{
    auto window = makeWindow();
    Q_EMIT engine->conversationsChanged({core::Conversation{.peer = kName,
                                                            .lastText = kName,
                                                            .lastAtMs = QDateTime::currentMSecsSinceEpoch(),
                                                            .lastOutgoing = false,
                                                            .unread = 0}});
    Q_EMIT engine->userListsChanged({core::ListedUser{.username = kName, .list = core::UserList::Banned}});
    ASSERT_TRUE(window->navigateTo(u"messages"_s));
    auto *list = child<QTreeView>(*window, u"messages.conversations"_s);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 1);
    EXPECT_EQ(shownToolTip(list->model()->index(0, 0).data(Qt::ToolTipRole)), kName);
    expectPeerMenu(contextMenuOn(list, list->model()->index(0, 0)));
    list->setCurrentIndex(list->model()->index(0, 0));
    EXPECT_EQ(shown(child<QLabel>(*window, u"messages.peer"_s)), kName);
    Q_EMIT engine->messageSent(kName, kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"messages.problem"_s)), kName);
    expectLabelsShowTheName(*window, 2);

    // Deleting it names the peer as it is.
    const auto *more = child<QPushButton>(*window, u"messages.more"_s);
    ASSERT_TRUE(more && more->menu());
    Q_EMIT more->menu()->aboutToShow();
    QAction *remove = nullptr;
    const QList<QAction *> actions = more->menu()->actions();
    for (QAction *action : actions) {
        if (action->text() == u"Delete Conversation…"_s) {
            remove = action;
        }
    }
    ASSERT_NE(remove, nullptr);
    remove->trigger();
    auto *confirm = child<QMessageBox>(*window, u"messages.deleteConfirm"_s);
    ASSERT_NE(confirm, nullptr);
    EXPECT_EQ(rendered(confirm->text(), confirm->textFormat()), u"Delete the conversation with %1?"_s.arg(kName));
    confirm->reject();

    // The people on the lists: the peer menu.
    ASSERT_TRUE(window->navigateTo(u"messages"_s));
    auto *people = child<QTreeView>(*window, u"people.table"_s);
    ASSERT_NE(people, nullptr);
    ASSERT_EQ(people->model()->rowCount(), 1);
    const QStringList texts = contextMenuOn(people, people->model()->index(0, 0));
    EXPECT_TRUE(texts.contains(u"Browse %1's Files"_s.arg(kName))) << texts.join(u" | "_s).toStdString();
}

TEST_F(NetworkTextTest, BrowseAndTheWindowTitle)
{
    auto window = makeWindow();
    window->browsePeer(kName);
    const QString title = u"%1's shares"_s.arg(kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"browse.headline"_s)), title);
    EXPECT_EQ(shown(child<QLabel>(*window, u"toolbar.title"_s)), title);
    for (const QString &name : {u"browse.details"_s, u"browse.waiting"_s, u"toolbar.subtitle"_s}) {
        EXPECT_EQ(child<QLabel>(*window, name)->textFormat(), Qt::PlainText) << name.toStdString();
    }
    expectLabelsShowTheName(*window, 2);

    // The sidebar's item for it.
    auto *sidebar = child<QListView>(*window, u"sidebar.list"_s);
    ASSERT_NE(sidebar, nullptr);
    bool found = false;
    for (int row = 0; row < sidebar->model()->rowCount(); ++row) {
        const QModelIndex index = sidebar->model()->index(row, 0);
        if (index.data(selecta::app::NavModel::IdRole).toString() == selecta::app::NavModel::browseItemId(kName)) {
            found = true;
            EXPECT_EQ(shownToolTip(index.data(Qt::ToolTipRole)), index.data().toString());
            EXPECT_TRUE(index.data().toString().contains(kName));
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(NetworkTextTest, Search)
{
    QSignalSpy started(engine.get(), &core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    auto *field = child<QLineEdit>(*window, u"sidebar.search"_s);
    ASSERT_NE(field, nullptr);
    field->setText(u"Okuma Drift"_s);
    QTest::keyClick(field, Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentViewId().startsWith(u"search:"_s); }, 5000));
    const qint64 searchId = window->currentViewId().mid(7).toLongLong();
    EXPECT_EQ(child<QLabel>(*window, u"search.releaseNote"_s)->textFormat(), Qt::PlainText);

    core::SearchHit hit;
    hit.peer = kName;
    hit.path = (u"@@x\\"_s + kName + u"\\Okuma Drift - Neon Rain.flac"_s).toUtf8();
    hit.extension = u"flac"_s;
    hit.size = 40'000'000;
    hit.attributes.duration = 372;
    core::SearchResult result = core::filterableResult(hit);
    result.index = 0;
    result.ownership = core::Ownership::InCollection;
    result.ownershipDetail = kName;
    const QList<core::SearchResult> results{result};
    auto *table = child<QTreeView>(*window, u"search.results"_s);
    ASSERT_NE(table, nullptr);
    // The running engine answers the search too (offline: nothing): give the results again until they stay.
    const auto shownRow = [&] {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            if (table->model()->index(row, 0).data().toString().contains(u"Neon Rain"_s)) {
                return true;
            }
        }
        Q_EMIT engine->searchResultsReset(searchId, results);
        return false;
    };
    ASSERT_TRUE(QTest::qWaitFor(shownRow, 5000));
    QTest::qWait(200);
    ASSERT_TRUE(QTest::qWaitFor(shownRow, 5000));
    const QModelIndex row = table->model()->index(0, 0);
    const QVariant peerTip = row.siblingAtColumn(selecta::app::SearchResultsModel::PeerColumn).data(Qt::ToolTipRole);
    EXPECT_TRUE(shownToolTip(peerTip).contains(kName)) << peerTip.toString().toStdString();
    const QVariant titleTip = row.data(Qt::ToolTipRole);
    EXPECT_TRUE(shownToolTip(titleTip).contains(kName)) << titleTip.toString().toStdString();
    expectPeerMenu(contextMenuOn(table, row));
    // The inspector: what of it you have.
    auto *ownership = child<QLabel>(*window, u"inspector.search.ownership"_s);
    ASSERT_NE(ownership, nullptr);
    EXPECT_EQ(shownToolTip(ownership->toolTip()), kName);
    expectLabelsShowTheName(*window, 2);
    engine->stop();
}

TEST(NetworkTextModels, SearchResultsShowNamesAsTheyAre)
{
    core::SearchHit hit;
    hit.peer = kName;
    hit.path = (u"@@x\\"_s + kName + u"\\Okuma Drift - Neon Rain.mp3"_s).toUtf8();
    hit.extension = u"mp3"_s;
    hit.size = 10'000'000;
    hit.attributes.bitrate = 320;
    hit.attributes.duration = 250;
    hit.freeSlot = true;
    core::SearchResult result = core::filterableResult(hit);
    result.index = 0;
    selecta::app::SearchResultsModel model;
    model.setGrouped(true);
    model.reset({result});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_GE(expectToolTipsShowTheName(model), 3); // the folder's path, its peer, the file's
}

TEST_F(NetworkTextTest, Activity)
{
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"activity"_s));
    // The log's sentences name peers.
    core::ActivityEntry entry;
    entry.id = 1;
    entry.atMs = QDateTime::currentMSecsSinceEpoch();
    entry.operation = u"browse"_s;
    entry.subject = kName;
    entry.outcome = u"allowed"_s;
    Q_EMIT engine->activityRecorded(entry);
    auto *log = child<QTreeView>(*window, u"activity.log"_s);
    ASSERT_NE(log, nullptr);
    ASSERT_GE(log->model()->rowCount(), 1);
    const QString sentence =
        shownToolTip(log->model()->index(0, selecta::app::ActivityModel::Details).data(Qt::ToolTipRole));
    EXPECT_TRUE(sentence.contains(kName)) << sentence.toStdString();
    // Spoken as written.
    EXPECT_FALSE(log->model()
                     ->index(0, selecta::app::ActivityModel::Details)
                     .data(Qt::AccessibleDescriptionRole)
                     .toString()
                     .contains(u"&lt;"_s));
}

TEST(NetworkTextModels, PeersShowNamesAsTheyAre)
{
    core::PeerStats peer;
    peer.peer = kName;
    peer.todayFiles = 3;
    selecta::app::PeersModel model;
    model.reset({peer}, QDateTime::currentMSecsSinceEpoch());
    ASSERT_EQ(model.rowCount(), 1);
    // One arg() for the whole sentence: the "%1" in the name stays.
    const QString limit = shownToolTip(model.index(0, selecta::app::PeersModel::LimitColumn).data(Qt::ToolTipRole));
    EXPECT_TRUE(limit.startsWith(u"3 files from %1 today, previews included. Selecta warns at "_s.arg(kName)))
        << limit.toStdString();
}

TEST(NetworkTextModels, TransfersShowNamesAsTheyAre)
{
    core::Download download;
    download.id = 1;
    download.peer = kName;
    download.remotePath = (u"@@x\\"_s + kName + u"\\01 Neon Rain.flac"_s).toUtf8();
    download.size = 10'000'000;
    download.state = core::Download::State::Failed;
    download.message = kName;
    selecta::app::TransfersModel model;
    model.reset({download}, {});
    model.setUploads(
        {core::UploadEntry{.transfer = 0, .user = kName, .path = kName, .size = 10, .sent = 0, .active = false}});
    model.flush();
    EXPECT_GE(expectToolTipsShowTheName(model), 3); // the file's path, why it failed, the upload's path
}

TEST(NetworkTextModels, GetTracksShowNamesAsTheyAre)
{
    core::GetLine line;
    line.id = 1;
    line.raw = kName;
    line.artist = u"Okuma Drift"_s;
    line.title = u"Neon Rain"_s;
    line.message = kName;
    core::FileChoice file;
    file.peer = kName;
    file.remotePath = (u"@@x\\"_s + kName + u"\\Neon Rain.mp3"_s).toUtf8();
    file.extension = u"mp3"_s;
    file.bitrate = 320;
    line.best = file;
    selecta::app::GetTracksModel model;
    model.reset({line});
    ASSERT_EQ(model.rowCount(), 1);
    using Model = selecta::app::GetTracksModel;
    EXPECT_TRUE(shownToolTip(model.index(0, Model::TrackColumn).data(Qt::ToolTipRole)).startsWith(kName + u'\n'));
    EXPECT_EQ(shownToolTip(model.index(0, Model::StatusColumn).data(Qt::ToolTipRole)), kName);
    EXPECT_EQ(model.index(0, Model::StatusColumn).data(Qt::AccessibleDescriptionRole).toString(), kName);
    EXPECT_EQ(shownToolTip(model.index(0, Model::FileColumn).data(Qt::ToolTipRole)),
              QString::fromUtf8(file.remotePath));
}

TEST_F(NetworkTextTest, GetTracks)
{
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"get"_s));
    Q_EMIT engine->tracksAdded(0, 0, 0, kName);
    const auto *note = child<QLabel>(*window, u"get.note"_s);
    EXPECT_TRUE(shown(note).contains(kName)) << note->text().toStdString();
    expectLabelsShowTheName(*window, 1);
}

TEST_F(NetworkTextTest, Collection)
{
    auto window = makeWindow();
    core::CollectionStatus status;
    status.roots = {dir.filePath(kName)};
    status.tracks = 1;
    status.formats = {{kName, 1}};
    status.unreadable = 1;
    status.unreadableSample = {{dir.filePath(u"a.mp3"_s), kName}};
    status.message = kName;
    Q_EMIT engine->collectionStatusChanged(status);
    core::SupersedeFlag flag;
    flag.id = 1;
    flag.album = kName;
    flag.single = kName;
    flag.singlePath = dir.filePath(kName + u".mp3"_s);
    flag.summary = kName;
    Q_EMIT engine->supersedeFlagsChanged({flag});
    ASSERT_TRUE(window->navigateTo(u"collection"_s));
    EXPECT_TRUE(shown(child<QLabel>(*window, u"collection.details"_s)).contains(kName));
    EXPECT_EQ(shown(child<QLabel>(*window, u"collection.message"_s)), kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"collection.formats"_s)), u"%1: 1"_s.arg(kName));
    expectLabelsShowTheName(*window, 3);

    const auto *unreadable = child<QListWidget>(*window, u"collection.unreadable"_s);
    ASSERT_TRUE(unreadable && unreadable->count() == 1);
    EXPECT_EQ(shownToolTip(unreadable->item(0)->toolTip()), kName);
    const auto *flags = child<QListWidget>(*window, u"collection.flags"_s);
    ASSERT_TRUE(flags && flags->count() == 1);
    EXPECT_TRUE(shownToolTip(flags->item(0)->toolTip()).endsWith(u'\n' + kName))
        << flags->item(0)->toolTip().toStdString();
}

TEST_F(NetworkTextTest, InboxAndItsInspector)
{
    auto window = makeWindow();
    core::InboxEntry entry;
    entry.item.id = 1;
    entry.item.kind = selecta::library::InboxItem::Kind::Single;
    entry.item.state = selecta::library::InboxItem::State::Ready;
    entry.item.relativePath = u"Singles/"_s + kName + u".mp3"_s;
    entry.item.title = kName;
    entry.item.artist = kName;
    entry.item.format = u"MP3"_s;
    entry.item.bitrate = 320;
    entry.item.peer = kName;
    entry.item.sourceFormat = u"FLAC 16/44.1"_s;
    entry.item.warning = kName;
    entry.alsoOn = {kName};
    Q_EMIT engine->inboxChanged({entry});
    ASSERT_TRUE(window->navigateTo(u"inbox"_s));
    auto *table = child<QTreeView>(*window, u"inbox.items"_s);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->model()->rowCount(), 1); // the Singles group
    const QModelIndex group = table->model()->index(0, 0);
    ASSERT_EQ(table->model()->rowCount(group), 1);
    EXPECT_GE(expectToolTipsShowTheName(*table->model()), 2); // the file's path, what the arrival noticed

    // The inspector says where it came from.
    table->selectionModel()->select(table->model()->index(0, 0, group),
                                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table->setCurrentIndex(table->model()->index(0, 0, group));
    const QString source = shown(child<QLabel>(*window, u"inspector.inbox.source"_s));
    EXPECT_TRUE(source.startsWith(u"Downloaded from %1 as FLAC 16/44.1."_s.arg(kName))) << source.toStdString();
    expectLabelsShowTheName(*window, 3);

    // What filing it said.
    Q_EMIT engine->inboxOperationFinished(1, false, kName);
    EXPECT_EQ(shown(child<QLabel>(*window, u"inbox.result"_s)), u"Couldn't do it: %1"_s.arg(kName));
}

TEST_F(NetworkTextTest, AnAlbumsReportNamesItsFolderAsItIs)
{
    const auto window = makeWindow();
    core::AlbumReport report;
    report.albumId = 1;
    report.album = kName;
    report.arrival = true;
    report.complete = true;
    report.tracks = 2;
    report.where = kName;
    report.trashed = {kName, kName};
    Q_EMIT engine->albumReported(report);
    auto *toasts = child<selecta::app::Toasts>(*window, u"toasts"_s);
    ASSERT_NE(toasts, nullptr);
    ASSERT_GE(toasts->count(), 1);
    const QString text = toasts->texts().constLast();
    EXPECT_TRUE(text.contains(u"2 tracks in Inbox/%1/"_s.arg(kName))) << text.toStdString();
    EXPECT_TRUE(text.contains(u"2 singles in your Inbox are on it, so they went to the Trash: %1, %1."_s.arg(kName)))
        << text.toStdString();
    expectLabelsShowTheName(*toasts, 1);
}

TEST_F(NetworkTextTest, TheReleasePickerShowsTheTrackAndWhatTheServicesSay)
{
    const selecta::app::ReleasePickerDialog dialog(*engine, 7, kName);
    EXPECT_EQ(shown(child<QLabel>(dialog, u"dialog.releasePicker.heading"_s)), u"Which release is “%1”?"_s.arg(kName));
    Q_EMIT engine->inboxReleasesFound(7, {}, kName);
    EXPECT_EQ(shown(child<QLabel>(dialog, u"dialog.releasePicker.status"_s)), kName);

    selecta::meta::Candidate candidate;
    candidate.release.title = kName;
    candidate.release.artists = {kName};
    candidate.reasons = {kName, u"same length"_s};
    Q_EMIT engine->inboxReleasesFound(7, {candidate}, {});
    const auto *list = child<QListWidget>(dialog, u"dialog.releasePicker.list"_s);
    ASSERT_TRUE(list && list->count() == 1);
    EXPECT_EQ(shownToolTip(list->item(0)->toolTip()), kName + u"\nsame length"_s);
}

TEST_F(NetworkTextTest, EditTagsNamesTheFileAsItIs)
{
    const selecta::app::EditTagsDialog dialog(kName, selecta::convert::MetadataModel{});
    EXPECT_EQ(shown(child<QLabel>(dialog, u"dialog.editTags.heading"_s)), u"Tags of “%1”"_s.arg(kName));
}

TEST_F(NetworkTextTest, Shares)
{
    auto window = makeWindow();
    core::ShareStatus status;
    status.roots = {dir.filePath(kName)};
    status.folders = 1;
    status.files = 1;
    status.message = kName;
    Q_EMIT engine->shareStatusChanged(status);
    Q_EMIT engine->uploadsChanged({core::UploadEntry{
        .transfer = 0, .user = kName, .path = u"@@x\\"_s + kName, .size = 10, .sent = 0, .active = false}});
    ASSERT_TRUE(window->navigateTo(u"shares"_s));
    EXPECT_TRUE(shown(child<QLabel>(*window, u"shares.folders"_s)).contains(kName));
    EXPECT_EQ(shown(child<QLabel>(*window, u"shares.message"_s)), kName);
    expectLabelsShowTheName(*window, 2);
    auto *uploads = child<QTreeWidget>(*window, u"shares.uploads"_s);
    ASSERT_TRUE(uploads && uploads->topLevelItemCount() == 1);
    EXPECT_EQ(shownToolTip(uploads->topLevelItem(0)->toolTip(1)), u"@@x\\"_s + kName);
    expectPeerMenu(contextMenuOn(uploads, uploads->model()->index(0, 0)));
}

TEST_F(NetworkTextTest, SettingsAndTheSidebarSayWhatTheServerSays)
{
    auto window = makeWindow();
    core::NetworkStatus status;
    status.state = core::NetworkStatus::State::NeedsUser;
    status.username = kName;
    status.detail = kName;
    Q_EMIT engine->networkStatusChanged(status);
    ASSERT_TRUE(window->navigateTo(u"settings"_s));
    EXPECT_EQ(shown(child<QLabel>(*window, u"settings.network.state"_s)), kName);
    for (const QString &name :
         {u"settings.network.portMappingState"_s, u"settings.network.reachability"_s, u"settings.preview.ytDlp.state"_s,
          u"settings.spotify.state"_s, u"settings.preview"_s, u"settings.libraryError"_s, u"sidebar.status"_s}) {
        EXPECT_EQ(child<QLabel>(*window, name)->textFormat(), Qt::PlainText) << name.toStdString();
    }
    expectLabelsShowTheName(*window, 1);
}

TEST(NetworkTextModels, ConverterShowsNamesAsTheyAre)
{
    core::ConverterItem item;
    item.id = 1;
    item.path = u"/tmp/"_s + kName + u".flac"_s;
    item.state = core::ConverterItem::State::Failed;
    item.message = kName;
    item.notices = {kName};
    selecta::app::ConverterModel model;
    model.setItems({item});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_GE(expectToolTipsShowTheName(model), 2); // the file, what came of it
}

TEST_F(NetworkTextTest, History)
{
    selecta::library::HistoryEntry entry;
    entry.session = session;
    entry.completedAt = 2'000;
    entry.peer = kName;
    entry.remotePath = (u"@@x\\"_s + kName + u".flac"_s).toUtf8();
    entry.quality = u"FLAC 16/44.1"_s;
    entry.artist = kName;
    entry.title = kName;
    entry.whereabouts = selecta::library::HistoryEntry::Whereabouts::Filed;
    entry.filedPath = dir.filePath(kName + u".flac"_s);
    ASSERT_TRUE(selecta::library::HistoryStore(db).add(entry).has_value());
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"history"_s));
    const auto *table = child<QAbstractItemView>(*window, u"history.sessions"_s);
    ASSERT_NE(table, nullptr);
    EXPECT_GE(expectToolTipsShowTheName(*table->model()), 2); // the peer's path, where it was filed
}

// The boxes Search asks with before downloading: the album offer, the album, and what you already have (DL-11, DL-12).
TEST_F(NetworkTextTest, SearchsQuestionsNameThePeerAndTheFolderAsTheyAre)
{
    QSignalSpy started(engine.get(), &core::EngineHost::started);
    engine->start();
    ASSERT_TRUE(started.wait(5000));
    auto window = makeWindow();
    auto *field = child<QLineEdit>(*window, u"sidebar.search"_s);
    ASSERT_NE(field, nullptr);
    field->setText(u"Okuma Drift"_s);
    QTest::keyClick(field, Qt::Key_Return);
    ASSERT_TRUE(QTest::qWaitFor([&] { return window->currentViewId().startsWith(u"search:"_s); }, 5000));
    const qint64 searchId = window->currentViewId().mid(7).toLongLong();

    // Three tracks of one folder, from a peer named kName; the first one owned. A folder's name is its path's last
    // part, so it has no "/".
    const QString folder = u"<i>x&y %1"_s;
    QList<core::SearchResult> results;
    for (int i = 0; i < 3; ++i) {
        core::SearchHit hit;
        hit.peer = kName;
        hit.path = (u"@@x\\"_s + folder + u"\\0%1 Okuma Drift - Neon Rain.flac"_s.arg(i + 1)).toUtf8();
        hit.extension = u"flac"_s;
        hit.size = 40'000'000;
        hit.attributes.duration = 372;
        core::SearchResult result = core::filterableResult(hit);
        result.index = i;
        result.peerFilesToday = 1'000; // past the warning: the album's box says so, naming the peer
        if (i == 0) {
            result.ownership = core::Ownership::InCollection;
            result.ownershipDetail = kName;
        }
        results.append(result);
    }
    ASSERT_EQ(results.constFirst().folder, u"@@x\\"_s + folder);
    auto *table = child<QTreeView>(*window, u"search.results"_s);
    ASSERT_NE(table, nullptr);
    const auto allShown = [&] {
        int files = 0;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const QModelIndex index = table->model()->index(row, 0);
            files += table->model()->rowCount(index) > 0 ? table->model()->rowCount(index) : 1;
        }
        if (files == 3) {
            return true;
        }
        Q_EMIT engine->searchResultsReset(searchId, results);
        return false;
    };
    ASSERT_TRUE(QTest::qWaitFor(allShown, 5000));
    QTest::qWait(200);
    ASSERT_TRUE(QTest::qWaitFor(allShown, 5000));
    table->setCurrentIndex(table->model()->index(0, 0));
    table->selectAll();

    // Download: three tracks of one folder, so the folder is offered as an album.
    child<QPushButton>(*window, u"search.download"_s)->click();
    QMessageBox *offer = openBox(*window, u"Download the whole folder as an album?"_s);
    ASSERT_NE(offer, nullptr);
    EXPECT_TRUE(shown(offer).contains(u"These 3 tracks are all in “%1” from %2."_s.arg(folder, kName)))
        << shown(offer).toStdString();

    // Just these files: one of them is yours already.
    child<QPushButton>(*offer, u"search.albumOffer.files"_s)->click();
    QMessageBox *owned = openBox(*window, u"You already have"_s);
    ASSERT_NE(owned, nullptr);
    EXPECT_TRUE(shown(owned).endsWith(u": "_s + kName)) << shown(owned).toStdString();
    owned->reject();

    // The folder as an album: what it takes from the peer today.
    table->setCurrentIndex(table->model()->index(0, 0));
    table->selectAll();
    contextMenu(table, table->visualRect(table->model()->index(0, 0)).center(), u"Download Folder as Album…"_s);
    QMessageBox *album = openBox(*window, u"Download “"_s);
    ASSERT_NE(album, nullptr);
    EXPECT_TRUE(shown(album).startsWith(u"Download “%1” as an album?"_s.arg(folder))) << shown(album).toStdString();
    EXPECT_TRUE(shown(album).contains(u"This takes you to 1003 files from %1 today"_s.arg(kName)))
        << shown(album).toStdString();
    album->reject();
    engine->stop();
}
