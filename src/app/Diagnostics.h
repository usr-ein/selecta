// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QImage;
class QTimer;

// What a beta tester's session leaves behind for the developer, in the log folder: the trail of what was done, the
// moments Selecta stopped answering, and the problems the tester noted. Nothing here leaves the machine.
namespace selecta::app {

// The user's actions as lines in the log (category selecta.app.trail), to read a bug report against what was done:
// clicks with the control's object name, rows picked in lists, shortcuts and menu choices, dialogs, and the application
// going to the background and back. Never what is typed: in a text field only Return, Escape and Tab are noted, never
// its text. Install on the application (QCoreApplication::installEventFilter); GUI thread only. It never consumes an
// event.
class ActionTrail : public QObject
{
    Q_OBJECT

public:
    explicit ActionTrail(QObject *parent = nullptr);

    // Notes the actions under `root` when they fire, however that happens: the macOS menu bar and its key equivalents
    // never reach the event filter. Actions made later aren't watched; clicks in their menus still are.
    void watchActions(QObject *root);

    // The trail's words for one event, or empty when it isn't worth a line. A key is told where it lands: at `focus`,
    // or at the window when nothing has the focus. Reads the widgets only; for tests.
    static QString describe(QObject *watched, QEvent *event, const QWidget *focus);

    bool eventFilter(QObject *watched, QEvent *event) override;
};

// Notices when a thread stops answering. A heartbeat timer runs on each watched thread; a watchdog thread looks at the
// beats every 50 ms. A stall longer than `threshold` is logged when it ends (category selecta.app.stall: which thread,
// how long, since when). A stall longer than `sampleAfter` also gets one stack sample of the whole process by macOS's
// /usr/bin/sample, written to `sampleDir` while it lasts (at most `maxSamples` a session), so a freeze can be read
// afterwards. Only while Selecta is the active application, by default. Create and destroy it on the GUI thread;
// watch() from the GUI thread or the watched one. Destroying it stops the watchdog thread (after a sample under way).
// A failed sample is logged and skipped. Elsewhere than macOS, stalls are only logged.
class StallWatchdog : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        std::chrono::milliseconds threshold{250};
        std::chrono::milliseconds sampleAfter{2000};
        int maxSamples = 10;
        QString sampleDir;           // empty: no samples
        bool onlyWhileActive = true; // in the background macOS may slow timers down (App Nap): not a stall
    };

    explicit StallWatchdog(Options options, QObject *parent = nullptr);
    ~StallWatchdog() override;
    StallWatchdog(const StallWatchdog &) = delete;
    StallWatchdog &operator=(const StallWatchdog &) = delete;
    StallWatchdog(StallWatchdog &&) = delete;
    StallWatchdog &operator=(StallWatchdog &&) = delete;

    // Watches the thread `context` lives in, under `name` ("GUI", "engine"). `context` must outlive the watchdog or
    // be destroyed with its thread; the heartbeat timer is its child, so it goes with it.
    void watch(QObject *context, const QString &name);

private:
    struct Beat
    {
        QString name;
        std::atomic<qint64> lastMs{0};
        qint64 stallSinceMs = 0; // watchdog thread only
        bool sampled = false;    // watchdog thread only
    };

    void run();
    void check(Beat &beat, qint64 nowMs);
    void sample(const Beat &beat, qint64 nowMs);

    const Options m_options;
    std::atomic<bool> m_active;
    std::mutex m_mutex; // guards m_beats' list (not the beats) and m_stop
    std::condition_variable m_wake;
    std::vector<std::shared_ptr<Beat>> m_beats;
    bool m_stop = false;
    int m_samples = 0; // watchdog thread only
    std::thread m_thread;
};

// "Note a Problem…": what the user wrote (note.txt), the window as it was before the note was asked for
// (screenshot.png), the app's state (state.json) and the end of the log (log-tail.txt), in a new folder
// <logDir>/feedback/<yyyy-MM-dd_HH-mm-ss>. Returns the folder, or an empty string with `error` set when it can't be
// written (whatever was written stays). GUI thread (it reads the log file, at most `tailBytes` of it).
QString saveFeedback(const QString &logDir, const QString &note, const QImage &screenshot, const QJsonObject &state,
                     QString *error, qint64 tailBytes = qint64{512} * 1024);

} // namespace selecta::app
