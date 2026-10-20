// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <optional>

namespace selecta::meta::detail {

// How one tool run ended, and what it printed.
struct ToolOutcome
{
    enum class End : quint8 {
        Exited,         // exitCode is the tool's
        Crashed,        // ended by a signal it didn't ask for
        FailedToStart,  // startError says why
        IdleTimeout,    // printed nothing for Spec::idleTimeout: killed
        TotalTimeout,   // ran longer than Spec::totalTimeout: killed
        Killed,         // kill() was called
        OutputTooLarge, // printed more than Spec::maxStdoutBytes: killed
    };

    End end = End::Exited;
    int exitCode = -1;
    QByteArray standardOutput;    // everything printed on stdout, within the budget
    QList<QByteArray> errorLines; // the last kMaxErrorLines stderr lines, each cut to kMaxLineBytes
    QString startError;
    std::chrono::milliseconds elapsed{0};
};

// One run of an external tool (yt-dlp): an argument list and no shell; the environment given, never the caller's;
// stdin closed; descriptors above stderr closed and signal handlers reset in the child; its own session, so a kill
// reaches everything the tool started. Output is split into lines within byte budgets, and an idle and a total
// timeout end a stuck run.
//
// Lines arrive through lineReady() as the tool prints them (when Spec::streamLines is set; a last unterminated line
// too); finished() follows exactly once, from the event loop, after the process has exited or failed to start, with
// outcome() filled in. kill() ends the process group; finished() still follows. Nothing is emitted inside start() or
// kill(). Destroying a running ToolProcess kills the group and reaps the process (a brief wait: SIGKILL can't be
// ignored).
//
// Threading: one thread, the owner's. Not thread-safe.
class ToolProcess final : public QObject
{
    Q_OBJECT

public:
    static constexpr qsizetype kMaxLineBytes = 4096;
    static constexpr qsizetype kMaxErrorLines = 200;

    struct Spec
    {
        QString program; // an absolute path
        QStringList arguments;
        QString workingDirectory;
        QProcessEnvironment environment;
        std::chrono::milliseconds idleTimeout{0};  // 0: none
        std::chrono::milliseconds totalTimeout{0}; // 0: none
        qint64 maxStdoutBytes = qint64{4} * 1024 * 1024;
        bool streamLines = false; // emit lineReady() for both channels
    };

    explicit ToolProcess(Spec spec, QObject *parent = nullptr);
    ~ToolProcess() override;
    ToolProcess(const ToolProcess &) = delete;
    ToolProcess &operator=(const ToolProcess &) = delete;
    ToolProcess(ToolProcess &&) = delete;
    ToolProcess &operator=(ToolProcess &&) = delete;

    void start();
    void kill();
    bool isRunning() const { return m_started && !m_done; }
    const ToolOutcome &outcome() const { return m_outcome; }

Q_SIGNALS:
    void lineReady(QProcess::ProcessChannel channel, const QByteArray &line);
    void finished();

private:
    void onReadyRead(QProcess::ProcessChannel channel);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    void takeLines(QProcess::ProcessChannel channel, bool flush);
    void handleLine(QProcess::ProcessChannel channel, QByteArray line);
    void killGroup(ToolOutcome::End reason);
    void complete();

    Spec m_spec;
    QProcess m_process;
    QTimer m_idleTimer;
    QTimer m_totalTimer;
    QElapsedTimer m_elapsed;
    QByteArray m_outPending; // the unterminated tail of each channel
    QByteArray m_errPending;
    ToolOutcome m_outcome;
    std::optional<ToolOutcome::End> m_killReason;
    bool m_started = false;
    bool m_done = false;
};

} // namespace selecta::meta::detail
