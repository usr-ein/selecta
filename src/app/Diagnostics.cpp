// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Diagnostics.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QShortcutEvent>
#include <QTextEdit>
#include <QThread>
#include <QTimer>

#include <array>

#ifdef Q_OS_MACOS
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

Q_LOGGING_CATEGORY(lcTrail, "selecta.app.trail")
Q_LOGGING_CATEGORY(lcStall, "selecta.app.stall")

constexpr qsizetype kMaxShownText = 60;
constexpr std::chrono::milliseconds kBeatInterval{50};
constexpr std::chrono::milliseconds kCheckInterval{50};
// A watchdog that wakes this much later than it asked was itself held up: the Mac slept or App Nap held the process.
// Nothing stalled, so every beat starts over.
constexpr std::chrono::milliseconds kSuspendedGap{1000};

qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QString shortText(QString text)
{
    text.replace(u'\n', u' ');
    text.remove(u'&'); // mnemonics
    return text.size() > kMaxShownText ? text.left(kMaxShownText - 1) + u'…' : text;
}

// The nearest object name a person (or the debug socket) would know the control by.
QString nameOf(const QObject *object)
{
    if (!object) {
        return {};
    }
    for (const QObject *o = object; o; o = o->parent()) {
        const QString name = o->objectName();
        if (!name.isEmpty() && !name.startsWith(u"qt_"_s)) {
            return name == object->objectName()
                       ? name
                       : name + u" › "_s + QString::fromLatin1(object->metaObject()->className());
        }
    }
    return QString::fromLatin1(object->metaObject()->className());
}

bool isTextInput(const QWidget *widget)
{
    if (!widget) {
        return false;
    }
    if (qobject_cast<const QLineEdit *>(widget) || qobject_cast<const QTextEdit *>(widget) ||
        qobject_cast<const QPlainTextEdit *>(widget) || qobject_cast<const QAbstractSpinBox *>(widget)) {
        return true;
    }
    const auto *combo = qobject_cast<const QComboBox *>(widget);
    return combo && combo->isEditable();
}

// Conversations are private: their lists say which row, never what it shows.
bool showsPrivateText(const QAbstractItemView *view)
{
    const QString name = view->objectName();
    return name.startsWith(u"messages."_s) || name.startsWith(u"rooms."_s);
}

QString describeClick(QWidget *widget, QPoint position)
{
    if (const auto *menu = qobject_cast<const QMenu *>(widget)) {
        const QAction *action = menu->actionAt(position);
        if (!action || action->isSeparator() || action->menu()) {
            return {};
        }
        return u"menu %1 \"%2\""_s.arg(action->objectName().isEmpty() ? u"-"_s : action->objectName(),
                                       shortText(action->text()));
    }
    if (const auto *view = qobject_cast<const QAbstractItemView *>(widget->parentWidget());
        view && view->viewport() == widget) {
        const QModelIndex index = view->indexAt(position);
        if (!index.isValid()) {
            return u"click %1 (empty space)"_s.arg(nameOf(view));
        }
        QString text = u"click %1 row %2 col %3"_s.arg(nameOf(view)).arg(index.row()).arg(index.column());
        if (index.parent().isValid()) {
            text += u" under row %1"_s.arg(index.parent().row());
        }
        if (!showsPrivateText(view)) {
            const QString shown = index.siblingAtColumn(0).data().toString();
            if (!shown.isEmpty()) {
                text += u": "_s + shortText(shown);
            }
        }
        return text;
    }
    QString text = u"click "_s + nameOf(widget);
    if (const auto *button = qobject_cast<const QAbstractButton *>(widget); button && !button->text().isEmpty()) {
        text += u" \""_s + shortText(button->text()) + u'"';
        if (button->isCheckable()) {
            text += button->isChecked() ? u" (now off)"_s : u" (now on)"_s; // the release toggles it next
        }
    }
    return text;
}

QString describeKey(const QWidget *widget, const QKeyEvent *key, const QWidget *focus)
{
    if (key->isAutoRepeat() || (focus ? widget != focus : !widget->isWindow())) {
        return {}; // once, where it lands: not again for each parent it's offered to
    }
    switch (key->key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_CapsLock:
        return {};
    default:
        break;
    }
    const bool command = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape ||
                         key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab;
    if (isTextInput(widget) && !command) {
        return {}; // typing: never what
    }
    const QString keys = QKeySequence(key->keyCombination()).toString(QKeySequence::PortableText);
    return u"key %1 in %2"_s.arg(keys, nameOf(widget));
}

} // namespace

// --- ActionTrail ---

ActionTrail::ActionTrail(QObject *parent)
    : QObject(parent)
{
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [](Qt::ApplicationState state) {
        qCInfo(lcTrail) << (state == Qt::ApplicationActive ? "app active" : "app in the background");
    });
}

void ActionTrail::watchActions(QObject *root)
{
    const QList<QAction *> actions = root->findChildren<QAction *>();
    for (const QAction *action : actions) {
        if (action->isSeparator()) {
            continue;
        }
        connect(action, &QAction::triggered, this, [action] {
            qCInfo(lcTrail).noquote() << u"action %1 \"%2\""_s.arg(
                action->objectName().isEmpty() ? u"-"_s : action->objectName(), shortText(action->text()));
        });
    }
}

QString ActionTrail::describe(QObject *watched, QEvent *event, const QWidget *focus)
{
    switch (event->type()) {
    case QEvent::MouseButtonRelease: {
        auto *widget = qobject_cast<QWidget *>(watched);
        const auto *mouse = dynamic_cast<const QMouseEvent *>(event);
        if (!widget || !mouse || mouse->button() != Qt::LeftButton ||
            !widget->rect().contains(mouse->position().toPoint())) {
            return {};
        }
        return describeClick(widget, mouse->position().toPoint());
    }
    case QEvent::KeyPress: {
        const auto *widget = qobject_cast<const QWidget *>(watched);
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        return widget && key ? describeKey(widget, key, focus) : QString();
    }
    case QEvent::Shortcut: {
        const auto *shortcut = dynamic_cast<const QShortcutEvent *>(event);
        return shortcut
                   ? u"shortcut %1 → %2"_s.arg(shortcut->key().toString(QKeySequence::PortableText), nameOf(watched))
                   : QString();
    }
    case QEvent::Show: {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || !widget->isWindow() || !qobject_cast<QDialog *>(widget)) {
            return {};
        }
        if (const auto *box = qobject_cast<const QMessageBox *>(widget)) {
            return u"dialog %1: \"%2\""_s.arg(nameOf(widget), shortText(box->text()));
        }
        return u"dialog %1 \"%2\""_s.arg(nameOf(widget), shortText(widget->windowTitle()));
    }
    default:
        return {};
    }
}

bool ActionTrail::eventFilter(QObject *watched, QEvent *event)
{
    if (const QString line = describe(watched, event, QApplication::focusWidget()); !line.isEmpty()) {
        qCInfo(lcTrail).noquote() << line;
    }
    return QObject::eventFilter(watched, event);
}

// --- StallWatchdog ---

StallWatchdog::StallWatchdog(Options options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_active(!m_options.onlyWhileActive || QGuiApplication::applicationState() == Qt::ApplicationActive)
{
    if (m_options.onlyWhileActive) {
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) { m_active.store(state == Qt::ApplicationActive); });
    }
    m_thread = std::thread([this] { run(); });
}

StallWatchdog::~StallWatchdog()
{
    {
        const std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void StallWatchdog::watch(QObject *context, const QString &name)
{
    const auto beat = std::make_shared<Beat>();
    beat->name = name;
    beat->lastMs.store(nowMs());
    {
        const std::scoped_lock lock(m_mutex);
        m_beats.push_back(beat);
    }
    // The timer lives on the watched thread, as the child of `context`: it beats as long as that thread's event loop
    // runs, and goes with `context`. The beat it holds outlives the watchdog if need be.
    const auto start = [context, beat] {
        auto *timer = new QTimer(context);
        timer->setInterval(kBeatInterval);
        QObject::connect(timer, &QTimer::timeout, timer, [beat] { beat->lastMs.store(nowMs()); });
        timer->start();
    };
    if (context->thread() == QThread::currentThread()) {
        start();
    } else {
        QMetaObject::invokeMethod(context, start, Qt::QueuedConnection);
    }
}

void StallWatchdog::run()
{
    qint64 previous = nowMs();
    std::vector<std::shared_ptr<Beat>> beats;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            if (m_wake.wait_for(lock, kCheckInterval, [this] { return m_stop; })) {
                return;
            }
            beats = m_beats; // checked without the lock: a sample takes a second or two
        }
        const qint64 now = nowMs();
        // In the background macOS may hold the process back (App Nap), and a sleeping Mac holds everything: neither
        // is a stall, so every beat starts over.
        const bool held = now - previous > kSuspendedGap.count() || !m_active.load();
        previous = now;
        for (const std::shared_ptr<Beat> &beat : beats) {
            if (held) {
                beat->lastMs.store(now);
                beat->stallSinceMs = 0;
                beat->sampled = false;
                continue;
            }
            check(*beat, now);
        }
    }
}

void StallWatchdog::check(Beat &beat, qint64 nowMs)
{
    const qint64 last = beat.lastMs.load();
    const qint64 silent = nowMs - last;
    if (silent <= m_options.threshold.count()) {
        if (beat.stallSinceMs > 0) {
            // It answers again: the stall lasted from the last beat before it to the first one after.
            const qint64 lasted = last - beat.stallSinceMs;
            const QDateTime since = QDateTime::currentDateTime().addMSecs(-(nowMs - beat.stallSinceMs));
            qCWarning(lcStall).noquote() << u"%1 thread stalled for %2 ms (from %3)%4"_s.arg(
                beat.name, QString::number(lasted), since.toString(u"HH:mm:ss.zzz"_s),
                beat.sampled ? u"; stack sample taken"_s : QString());
            beat.stallSinceMs = 0;
            beat.sampled = false;
        }
        return;
    }
    if (beat.stallSinceMs == 0) {
        beat.stallSinceMs = last;
    }
    if (!beat.sampled && silent >= m_options.sampleAfter.count() && !m_options.sampleDir.isEmpty() &&
        m_samples < m_options.maxSamples) {
        beat.sampled = true;
        ++m_samples;
        sample(beat, nowMs);
    }
}

void StallWatchdog::sample(const Beat &beat, qint64 nowMs)
{
#ifdef Q_OS_MACOS
    if (!QDir().mkpath(m_options.sampleDir)) {
        qCWarning(lcStall) << "can't create" << m_options.sampleDir;
        return;
    }
    const QString path = QDir(m_options.sampleDir)
                             .filePath(u"stall-%1-%2.txt"_s.arg(
                                 QDateTime::currentDateTime().toString(u"yyyy-MM-dd_HH-mm-ss"_s), beat.name.toLower()));
    qCWarning(lcStall).noquote() << u"%1 thread silent for %2 ms: sampling the process into %3"_s.arg(
        beat.name, QString::number(nowMs - beat.lastMs.load()), path);
    // sample <pid> <seconds> -file <path>: one second of the whole process, every thread's stack.
    QByteArray program("sample");
    QByteArray pid = QByteArray::number(static_cast<qint64>(getpid()));
    QByteArray seconds("1");
    QByteArray fileOption("-file");
    QByteArray file = path.toUtf8();
    std::array<char *, 6> argv{program.data(), pid.data(), seconds.data(), fileOption.data(), file.data(), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, "/usr/bin/sample", nullptr, nullptr, argv.data(), nullptr) != 0) {
        qCWarning(lcStall) << "couldn't run /usr/bin/sample";
        return;
    }
    int status = 0;
    waitpid(child, &status, 0); // on the watchdog thread: only it waits
#else
    Q_UNUSED(beat)
    Q_UNUSED(nowMs)
#endif
}

// --- Feedback ---

QString saveFeedback(const QString &logDir, const QString &note, const QImage &screenshot, const QJsonObject &state,
                     QString *error, qint64 tailBytes)
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString base = QDir(logDir).filePath(u"feedback/"_s + now.toString(u"yyyy-MM-dd_HH-mm-ss"_s));
    QString folder = base;
    for (int n = 2; QFileInfo::exists(folder); ++n) {
        folder = base + u'-' + QString::number(n);
    }
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return QString();
    };
    if (!QDir().mkpath(folder)) {
        return fail(u"can't create %1"_s.arg(folder));
    }
    const auto write = [&folder](const QString &name, const QByteArray &bytes) {
        QFile file(QDir(folder).filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write(u"note.txt"_s, (u"Noted at "_s + now.toString(Qt::ISODateWithMs) + u"\n\n"_s + note + u'\n').toUtf8())) {
        return fail(u"can't write the note in %1"_s.arg(folder));
    }
    if (!screenshot.isNull() && !screenshot.save(QDir(folder).filePath(u"screenshot.png"_s))) {
        return fail(u"can't save the screenshot in %1"_s.arg(folder));
    }
    if (!write(u"state.json"_s, QJsonDocument(state).toJson(QJsonDocument::Indented))) {
        return fail(u"can't write the state in %1"_s.arg(folder));
    }
    QFile log(QDir(logDir).filePath(u"selecta.log"_s));
    if (log.open(QIODevice::ReadOnly)) {
        const qint64 start = std::max<qint64>(0, log.size() - tailBytes);
        log.seek(start);
        QByteArray tail = log.read(tailBytes);
        if (start > 0) {
            tail.remove(0, tail.indexOf('\n') + 1); // from the first whole line
        }
        if (!write(u"log-tail.txt"_s, tail)) {
            return fail(u"can't write the log's end in %1"_s.arg(folder));
        }
    }
    return folder;
}

} // namespace selecta::app
