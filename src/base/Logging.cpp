// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcBase, "selecta.base")

namespace selecta::base {
namespace {

// Guarded by `mutex`. Deliberately leaked: Qt may log during static destruction, after a function-local static would
// already be gone.
struct LoggerState
{
    QMutex mutex;
    bool installed = false;
    QString dir;
    qint64 maxBytes = 0;
    int keepFiles = 0;
    QFile file;
    qint64 written = 0;
};

LoggerState &state()
{
    static auto *s = new LoggerState;
    return *s;
}

// The handler that was active before ours. Atomic so a nested message (see t_inHandler) can reach it without the mutex.
std::atomic<QtMessageHandler> g_previous{nullptr};

// Set while this thread is inside the handler. A message logged from inside it (e.g. by QFile) must not try to take the
// mutex again; it goes to the previous handler only.
thread_local bool t_inHandler = false;

QString logPath(const QString &dir, int index)
{
    return index == 0 ? dir + u"/selecta.log"_s : dir + u"/selecta.%1.log"_s.arg(index);
}

bool openCurrent(LoggerState &s)
{
    s.file.setFileName(logPath(s.dir, 0));
    if (!s.file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    s.written = s.file.size();
    return true;
}

// selecta.log → selecta.1.log → … → selecta.<keep-1>.log; the oldest is dropped.
void rotate(LoggerState &s)
{
    s.file.close();
    QFile::remove(logPath(s.dir, s.keepFiles - 1));
    for (int i = s.keepFiles - 2; i >= 0; --i)
        QFile::rename(logPath(s.dir, i), logPath(s.dir, i + 1));
    openCurrent(s); // if this fails, file logging stops; the previous handler still gets every message
}

QLatin1StringView levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "debug"_L1;
    case QtInfoMsg:
        return "info"_L1;
    case QtWarningMsg:
        return "warning"_L1;
    case QtCriticalMsg:
        return "critical"_L1;
    case QtFatalMsg:
        return "fatal"_L1;
    }
    return "unknown"_L1;
}

void handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (!t_inHandler) {
        t_inHandler = true;
        LoggerState &s = state();
        {
            QMutexLocker locker(&s.mutex);
            if (s.installed && s.file.isOpen()) {
                const QByteArray line =
                    (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + u' ' + levelName(type) + u' ' +
                     QLatin1StringView(context.category ? context.category : "default") + u": " + message + u'\n')
                        .toUtf8();
                if (s.file.write(line) == line.size())
                    s.written += line.size();
                s.file.flush(); // flushed per message so a crash keeps the last lines
                if (s.written >= s.maxBytes && s.keepFiles > 1)
                    rotate(s);
            }
        }
        t_inHandler = false;
    }
    if (QtMessageHandler previous = g_previous.load(std::memory_order_acquire))
        previous(type, context, message);
}

} // namespace

bool installFileLogger(const QString &dir, qint64 maxBytes, int keepFiles)
{
    if (!QDir().mkpath(dir))
        return false;

    bool opened = false;
    {
        LoggerState &s = state();
        QMutexLocker locker(&s.mutex);
        s.file.close();
        s.dir = dir;
        s.maxBytes = maxBytes > 0 ? maxBytes : qint64{1} << 20;
        s.keepFiles = keepFiles > 0 ? keepFiles : 1;
        opened = openCurrent(s);
        s.installed = true;
    }

    // Installing twice must not record our own handler as "previous": it would call itself forever.
    const QtMessageHandler previous = qInstallMessageHandler(handleMessage);
    if (previous != handleMessage)
        g_previous.store(previous, std::memory_order_release);
    return opened;
}

void uninstallFileLogger()
{
    LoggerState &s = state();
    {
        QMutexLocker locker(&s.mutex);
        if (!s.installed)
            return;
        s.installed = false;
        s.file.close();
    }
    // New messages go straight to the previous handler from here on. A message already inside handleMessage on
    // another thread finishes safely: the state is never destroyed and `installed` is now false.
    qInstallMessageHandler(g_previous.load(std::memory_order_acquire));
}

} // namespace selecta::base
