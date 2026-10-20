// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceOver and the keyboard over the whole window (PRD UI-5): every view's controls have a name to read, and the
// keyboard reaches them.
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "app/Preview.h"
#include "app/Toasts.h"
#include "app/WaveformView.h"
#include "app/views/AddTracksDialog.h"
#include "app/views/EditTagsDialog.h"
#include "app/views/ReleasePickerDialog.h"
#include "convert/MetadataModel.h"
#include "core/EngineHost.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"
#include "support/FakeNotificationSink.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::AppSettings;
using selecta::app::MainWindow;

namespace {

class AccessibilityTest : public ::testing::Test
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
        engine = std::make_unique<selecta::core::EngineHost>(options);       // never started: no network
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

// A control the user acts on, as opposed to the parts Qt builds into one (a combo box's line edit, a table's header,
// scroll bars).
bool isControl(const QWidget *widget)
{
    if (qobject_cast<const QScrollBar *>(widget) != nullptr || qobject_cast<const QHeaderView *>(widget) != nullptr) {
        return false;
    }
    const QWidget *parent = widget->parentWidget();
    if (qobject_cast<const QComboBox *>(parent) != nullptr ||
        qobject_cast<const QAbstractSpinBox *>(parent) != nullptr) {
        return false;
    }
    if (widget->objectName().startsWith(u"qt_"_s)) { // Qt's own: a toolbar's extension button, a scroll area's corner
        return false;
    }
    return qobject_cast<const QAbstractButton *>(widget) != nullptr ||
           qobject_cast<const QComboBox *>(widget) != nullptr || qobject_cast<const QLineEdit *>(widget) != nullptr ||
           qobject_cast<const QAbstractSlider *>(widget) != nullptr ||
           qobject_cast<const QAbstractSpinBox *>(widget) != nullptr ||
           qobject_cast<const QAbstractItemView *>(widget) != nullptr ||
           qobject_cast<const QTextEdit *>(widget) != nullptr ||
           qobject_cast<const QPlainTextEdit *>(widget) != nullptr ||
           qobject_cast<const selecta::app::WaveformView *>(widget) != nullptr;
}

QString describe(QWidget *widget)
{
    QString text = u"%1 \"%2\""_s.arg(QString::fromLatin1(widget->metaObject()->className()), widget->objectName());
    for (const QWidget *parent = widget->parentWidget(); parent != nullptr && parent->objectName().isEmpty();
         parent = parent->parentWidget()) {
        text += u" in "_s + QString::fromLatin1(parent->metaObject()->className());
    }
    if (const QWidget *named = widget->parentWidget(); named != nullptr) {
        QString owner;
        for (const QWidget *parent = named; parent != nullptr; parent = parent->parentWidget()) {
            if (!parent->objectName().isEmpty()) {
                owner = parent->objectName();
                break;
            }
        }
        text += u" (in "_s + owner + u")"_s;
    }
    if (const QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(widget)) {
        text += u" named \""_s + iface->text(QAccessible::Name) + u'"';
    }
    return text;
}

// Every control in `window`, shown or not: a table behind an empty state counts too. Popups (a combo box's list)
// are windows of their own and left out.
QList<QWidget *> controlsOf(QWidget &window)
{
    QList<QWidget *> found;
    const QList<QWidget *> all = window.findChildren<QWidget *>();
    for (QWidget *widget : all) {
        if (widget->window() == &window && isControl(widget)) {
            found.append(widget);
        }
    }
    return found;
}

QList<QWidget *> controls(MainWindow &window)
{
    // Views may build parts the first time they're shown.
    const QStringList ids = window.viewIds();
    for (const QString &id : ids) {
        window.navigateTo(id);
        QApplication::processEvents();
    }
    return controlsOf(window);
}

// Each control has a name VoiceOver can read, but those in `notYet`.
void expectNames(const QWidget &window, const QList<QWidget *> &found, const QStringList &notYet = {})
{
    for (QWidget *control : found) {
        if (notYet.contains(control->objectName()) ||
            (control->parentWidget() != nullptr && notYet.contains(control->parentWidget()->objectName()))) {
            continue;
        }
        // A text button that is hidden until its view fills it in ("Open in Spotify"), not an icon-only one.
        const auto *button = qobject_cast<QAbstractButton *>(control);
        if (button != nullptr && !control->isVisibleTo(&window) && button->icon().isNull()) {
            continue;
        }
        QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(control);
        ASSERT_NE(iface, nullptr) << describe(control).toStdString();
        if (qobject_cast<QComboBox *>(control) != nullptr) {
            // On macOS Qt reads a pop-up's name as its current item; VoiceOver takes the label from the field's
            // buddy, so a pop-up needs one, or at least a name for the other platforms.
            EXPECT_TRUE(!iface->relations(QAccessible::Label).isEmpty() || !control->accessibleName().isEmpty())
                << describe(control).toStdString();
            continue;
        }
        EXPECT_FALSE(iface->text(QAccessible::Name).trimmed().isEmpty()) << describe(control).toStdString();
    }
}

} // namespace

TEST_F(AccessibilityTest, EveryControlHasAName)
{
    // Waiting for the teammates who are in these files (InboxView, InboxInspector, SettingsView, ConverterView).
    const QStringList notYet{u"inbox.items"_s,           u"inspector.inbox.openRelease"_s,
                             u"converter.items"_s,       u"converter.artwork"_s,
                             u"converter.id3"_s,         u"settings.dataDir"_s,
                             u"settings.inbox"_s,        u"settings.logDir"_s,
                             u"settings.network.host"_s, u"settings.preview.ytDlp"_s,
                             u"settings.collections"_s,  u"settings.network.port"_s};
    auto window = makeWindow();
    const QList<QWidget *> found = controls(*window);
    EXPECT_GT(found.size(), 100); // the walk found the views' controls
    expectNames(*window, found, notYet);
}

TEST_F(AccessibilityTest, EveryControlInTheDialogsHasAName)
{
    selecta::app::AddTracksDialog add;
    EXPECT_FALSE(controlsOf(add).isEmpty());
    expectNames(add, controlsOf(add));
    const selecta::convert::MetadataModel tags;
    selecta::app::EditTagsDialog editTags(u"01 Neon Rain.mp3"_s, tags);
    expectNames(editTags, controlsOf(editTags));
    selecta::app::ReleasePickerDialog picker(*engine, 1, u"Okuma Drift - Neon Rain"_s);
    expectNames(picker, controlsOf(picker));
}

TEST_F(AccessibilityTest, TheKeyboardReachesEveryControl)
{
    // Kept out of the Tab chain on purpose, as on a Mac:
    // - the player's transport and volume: Space, ⌥← and ⌥→ work from anywhere, and a focused button would take Space
    //   from the window;
    // - the toolbar's buttons: each one's command is in a menu, with its shortcut;
    // - a search field's clear button: Escape and ⌘⌫ clear the field.
    const QStringList outOfTheChain{u"player.back"_s, u"player.play"_s, u"player.forward"_s, u"player.volume"_s,
                                    u"player.discard"_s};
    auto window = makeWindow();
    const QList<QWidget *> found = controls(*window);
    for (QWidget *control : found) {
        if (outOfTheChain.contains(control->objectName()) || control->parentWidget()->objectName() == u"toolbar"_s ||
            qobject_cast<QLineEdit *>(control->parentWidget()) != nullptr) {
            continue;
        }
        EXPECT_TRUE(control->focusPolicy() & Qt::TabFocus) << describe(control).toStdString();
    }
}

TEST(Accessibility, ToastsAreReadOutAndWaitForTheKeyboard)
{
    static QStringList announced;
    announced.clear();
    QAccessible::installUpdateHandler([](QAccessibleEvent *event) {
        if (const auto *announcement = dynamic_cast<QAccessibleAnnouncementEvent *>(event)) {
            announced.append(announcement->message());
        }
    });
    QWidget host;
    host.resize(600, 400);
    host.setAttribute(Qt::WA_DontShowOnScreen);
    host.show();
    selecta::app::Toasts toasts(&host);
    const QString name =
        toasts.show(u"Moved 2 items to the Trash"_s, u"From your Inbox"_s, selecta::app::Toasts::Kind::Info,
                    {{u"Undo"_s, [] {}}}, std::chrono::milliseconds(50));
    QAccessible::installUpdateHandler(nullptr);
    EXPECT_EQ(announced, QStringList{u"Moved 2 items to the Trash\nFrom your Inbox"_s});

    // Focus on Undo holds it past its timeout.
    auto *undo = toasts.findChild<QPushButton *>(name + u".action.0"_s);
    ASSERT_NE(undo, nullptr);
    QFocusEvent focusIn(QEvent::FocusIn, Qt::TabFocusReason);
    QApplication::sendEvent(undo, &focusIn);
    QTest::qWait(120);
    EXPECT_EQ(toasts.count(), 1);
    QFocusEvent focusOut(QEvent::FocusOut, Qt::TabFocusReason);
    QApplication::sendEvent(undo, &focusOut);
    QTRY_COMPARE_WITH_TIMEOUT(toasts.count(), 0, 2000);
}

TEST_F(AccessibilityTest, TabGoesFromTheSidebarToTheViewThenThePlayer)
{
    auto window = makeWindow();
    for (const QString &id : {u"get"_s, u"wishlist"_s, u"rooms"_s}) {
        window->navigateTo(id);
        QApplication::processEvents();
        QStringList chain;
        const QWidget *next = window.get();
        for (int step = 0; step < 5000; ++step) {
            next = next->nextInFocusChain();
            if (next == window.get()) {
                break;
            }
            if (next->isVisibleTo(window.get()) && next->isEnabled() && (next->focusPolicy() & Qt::TabFocus)) {
                chain.append(next->objectName());
            }
        }
        const auto at = [&chain](const QString &name) { return chain.indexOf(name); };
        ASSERT_GE(at(u"sidebar.search"_s), 0) << chain.join(u' ').toStdString();
        EXPECT_LT(at(u"sidebar.search"_s), at(u"sidebar.list"_s));
        const QWidget *view = window->findChild<QWidget *>(u"view."_s + id);
        ASSERT_NE(view, nullptr);
        int inView = 0;
        for (const QString &name : std::as_const(chain)) {
            const auto *widget = window->findChild<QWidget *>(name);
            if (widget != nullptr && view->isAncestorOf(widget)) {
                ++inView;
                EXPECT_GT(at(name), at(u"sidebar.list"_s)) << name.toStdString();
                EXPECT_LT(at(name), at(u"player.waveform"_s)) << name.toStdString();
            }
        }
        EXPECT_GT(inView, 0) << id.toStdString();
    }
}
