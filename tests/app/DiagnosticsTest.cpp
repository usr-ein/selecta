// SPDX-License-Identifier: GPL-3.0-or-later
// What a tester's session leaves for the developer: the action trail, stalls, and problem notes (Diagnostics.h); and
// the live network's switch in Settings (SAFE-3), which the tester's build shows.
#include "app/Diagnostics.h"

#include "app/views/SettingsView.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMutex>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <gtest/gtest.h>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::app::ActionTrail;
using selecta::app::SettingsView;
using selecta::app::StallWatchdog;

namespace {

QString release(QWidget *widget, QPoint at)
{
    QMouseEvent event(QEvent::MouseButtonRelease, at, widget->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
    return ActionTrail::describe(widget, &event, nullptr);
}

QString press(QWidget *widget, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    return ActionTrail::describe(widget, &event, widget); // as the focus widget
}

// Messages logged in one category, from any thread.
class CategoryCapture
{
public:
    explicit CategoryCapture(const char *category)
        : m_previous([category] {
            s_category = category;
            s_lines.clear();
            return qInstallMessageHandler(&CategoryCapture::handle);
        }())
    {}
    ~CategoryCapture() { qInstallMessageHandler(m_previous); }
    CategoryCapture(const CategoryCapture &) = delete;
    CategoryCapture &operator=(const CategoryCapture &) = delete;
    CategoryCapture(CategoryCapture &&) = delete;
    CategoryCapture &operator=(CategoryCapture &&) = delete;

    static QStringList lines()
    {
        const QMutexLocker locker(&s_mutex);
        return s_lines;
    }

private:
    static void handle([[maybe_unused]] QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        if (context.category && qstrcmp(context.category, s_category) == 0) {
            const QMutexLocker locker(&s_mutex);
            s_lines.append(message);
        }
    }

    QtMessageHandler m_previous = nullptr;
    static inline QMutex s_mutex;
    static inline QStringList s_lines;
    static inline const char *s_category = "";
};

} // namespace

TEST(ActionTrail, NamesTheControlNeverWhatIsTyped)
{
    QWidget window;
    window.setObjectName(u"window"_s);
    auto *save = new QPushButton(u"&Save"_s, &window);
    save->setObjectName(u"settings.network.save"_s);
    auto *field = new QLineEdit(&window);
    field->setObjectName(u"settings.network.password"_s);
    field->setEchoMode(QLineEdit::Password);
    auto *list = new QListWidget(&window);
    list->setObjectName(u"get.lines"_s);
    list->addItems({u"Kel Hale - Hollow Tide"_s, u"DJ Harbour - Amber Signal"_s});
    window.resize(400, 300);
    list->setGeometry(0, 100, 400, 200);
    window.show();

    EXPECT_EQ(release(save, QPoint(5, 5)), u"click settings.network.save \"Save\""_s);
    // A row picked in a list: which one, and what it shows.
    const QRect second = list->visualItemRect(list->item(1));
    EXPECT_EQ(release(list->viewport(), second.center()), u"click get.lines row 1 col 0: DJ Harbour - Amber Signal"_s);
    // Typing is never noted; only the keys that act are.
    EXPECT_TRUE(press(field, Qt::Key_A).isEmpty());
    EXPECT_TRUE(press(field, Qt::Key_Space).isEmpty());
    EXPECT_EQ(press(field, Qt::Key_Return), u"key Return in settings.network.password"_s);
    EXPECT_EQ(press(list, Qt::Key_Space), u"key Space in get.lines"_s);
    EXPECT_EQ(press(list, Qt::Key_K), u"key K in get.lines"_s);
    EXPECT_TRUE(press(list, Qt::Key_Shift).isEmpty());
    // Only where the key lands, not again in each parent it's offered to.
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    EXPECT_TRUE(ActionTrail::describe(&window, &space, list).isEmpty());
}

TEST(StallWatchdog, NoticesAThreadThatStopsAnswering)
{
    const CategoryCapture capture("selecta.app.stall");
    StallWatchdog watchdog(
        {.threshold = 150ms, .sampleAfter = 60s, .maxSamples = 0, .sampleDir = {}, .onlyWhileActive = false});
    QObject context;
    watchdog.watch(&context, u"GUI"_s);
    QTest::qWait(300); // beating

    std::this_thread::sleep_for(700ms); // this thread stops answering
    ASSERT_TRUE(QTest::qWaitFor(
        [] {
            const QStringList lines = CategoryCapture::lines();
            return std::ranges::any_of(lines, [](const QString &line) { return line.contains(u"stalled"_s); });
        },
        3000));
    // The stall is told once it's over, with how long it lasted (busy machines add a little).
    bool found = false;
    const QStringList lines = CategoryCapture::lines();
    for (const QString &line : lines) {
        static const QRegularExpression stalled(u"^GUI thread stalled for (\\d+) ms"_s);
        const QRegularExpressionMatch match = stalled.match(line);
        if (match.hasMatch() && match.captured(1).toInt() >= 600) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << CategoryCapture::lines().join(u" | "_s).toStdString();
}

TEST(Feedback, KeepsTheNoteTheWindowTheStateAndTheEndOfTheLog)
{
    const QTemporaryDir logs;
    ASSERT_TRUE(logs.isValid());
    {
        QFile log(logs.filePath(u"selecta.log"_s));
        ASSERT_TRUE(log.open(QIODevice::WriteOnly));
        log.write("first line, long enough to be cut\nsecond line\nthird line\n");
    }
    QImage screenshot(40, 30, QImage::Format_RGB32);
    screenshot.fill(Qt::darkCyan);
    QString error;
    const QString folder = selecta::app::saveFeedback(logs.path(), u"The preview didn't start."_s, screenshot,
                                                      QJsonObject{{"view"_L1, u"get"_s}}, &error, 30);
    ASSERT_FALSE(folder.isEmpty()) << error.toStdString();
    EXPECT_TRUE(folder.startsWith(logs.filePath(u"feedback/"_s)));

    QFile note(QDir(folder).filePath(u"note.txt"_s));
    ASSERT_TRUE(note.open(QIODevice::ReadOnly));
    EXPECT_TRUE(QString::fromUtf8(note.readAll()).contains(u"The preview didn't start."_s));
    EXPECT_EQ(QImage(QDir(folder).filePath(u"screenshot.png"_s)).size(), QSize(40, 30));
    QFile state(QDir(folder).filePath(u"state.json"_s));
    ASSERT_TRUE(state.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(state.readAll()).object().value("view"_L1).toString(), u"get"_s);
    // The log's last bytes, from the first whole line.
    QFile tail(QDir(folder).filePath(u"log-tail.txt"_s));
    ASSERT_TRUE(tail.open(QIODevice::ReadOnly));
    EXPECT_EQ(tail.readAll(), QByteArray("second line\nthird line\n"));

    // A second note in the same second gets a folder of its own.
    const QString again = selecta::app::saveFeedback(logs.path(), u"Again."_s, screenshot, QJsonObject{}, &error, 30);
    ASSERT_FALSE(again.isEmpty()) << error.toStdString();
    EXPECT_NE(again, folder);
}

TEST(LiveNetworkSwitch, OnlyABuildThatMayReachItShowsItAndItFillsInTheServer)
{
    selecta::core::NetworkConfig config;
    config.username = u"selecta_beta"_s;
    // A build without the live network compiled in has no switch at all.
    SettingsView local(selecta::app::BuildInfo{}, selecta::app::Appearance::System, selecta::core::LibraryConfig{},
                       config);
    EXPECT_EQ(local.findChild<QCheckBox *>(u"settings.network.live"_s), nullptr);

    selecta::app::BuildInfo live;
    live.liveServerCompiledIn = true;
    SettingsView view(live, selecta::app::Appearance::System, selecta::core::LibraryConfig{}, config);
    auto *box = view.findChild<QCheckBox *>(u"settings.network.live"_s);
    ASSERT_NE(box, nullptr);
    EXPECT_FALSE(box->isChecked()); // the user asks for it (SAFE-3)
    QList<selecta::core::NetworkConfig> changed;
    QObject::connect(&view, &SettingsView::networkConfigChanged, &view,
                     [&changed](const selecta::core::NetworkConfig &saved) { changed.append(saved); });
    box->setChecked(true);
    EXPECT_EQ(view.findChild<QLineEdit *>(u"settings.network.host"_s)->text(), u"server.slsknet.org"_s);
    EXPECT_EQ(view.findChild<QSpinBox *>(u"settings.network.port"_s)->value(), 2242);
    view.findChild<QPushButton *>(u"settings.network.save"_s)->click();
    ASSERT_EQ(changed.size(), 1);
    EXPECT_TRUE(changed.constFirst().liveAllowedByUser);
    EXPECT_EQ(changed.constFirst().host, u"server.slsknet.org"_s);
}
