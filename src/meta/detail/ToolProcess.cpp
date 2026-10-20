// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ToolProcess.h"

#include <QMetaObject>

#include <csignal>
#include <sys/types.h>
#include <utility>

namespace selecta::meta::detail {

namespace {

// An unterminated line longer than this is cut and taken as a line (stderr, or stdout when streaming lines).
constexpr qsizetype kMaxPendingBytes = qsizetype{64} * 1024;

} // namespace

ToolProcess::ToolProcess(Spec spec, QObject *parent)
    : QObject(parent)
    , m_spec(std::move(spec))
{
    m_idleTimer.setSingleShot(true);
    m_totalTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] { killGroup(ToolOutcome::End::IdleTimeout); });
    connect(&m_totalTimer, &QTimer::timeout, this, [this] { killGroup(ToolOutcome::End::TotalTimeout); });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { onReadyRead(QProcess::StandardOutput); });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { onReadyRead(QProcess::StandardError); });
    connect(&m_process, &QProcess::finished, this, &ToolProcess::onFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &ToolProcess::onError);
}

ToolProcess::~ToolProcess()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.disconnect(this);
        const qint64 pid = m_process.processId();
        if (pid > 0) {
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
        }
        m_process.kill();
        m_process.waitForFinished(5000); // reaps it; SIGKILL can't be caught or ignored
    }
}

void ToolProcess::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_process.setProgram(m_spec.program);
    m_process.setArguments(m_spec.arguments);
    m_process.setProcessEnvironment(m_spec.environment);
    m_process.setWorkingDirectory(m_spec.workingDirectory);
    m_process.setStandardInputFile(QProcess::nullDevice());
    QProcess::UnixProcessParameters parameters;
    parameters.flags = QProcess::UnixProcessFlag::CloseFileDescriptors | QProcess::UnixProcessFlag::CreateNewSession |
                       QProcess::UnixProcessFlag::ResetSignalHandlers;
    m_process.setUnixProcessParameters(parameters);
    m_elapsed.start();
    if (m_spec.idleTimeout.count() > 0) {
        m_idleTimer.start(m_spec.idleTimeout);
    }
    if (m_spec.totalTimeout.count() > 0) {
        m_totalTimer.start(m_spec.totalTimeout);
    }
    m_process.start(QIODevice::ReadOnly);
}

void ToolProcess::kill()
{
    killGroup(ToolOutcome::End::Killed);
}

void ToolProcess::killGroup(ToolOutcome::End reason)
{
    if (!m_started || m_done) {
        return;
    }
    if (!m_killReason.has_value()) {
        m_killReason = reason;
    }
    m_idleTimer.stop();
    m_totalTimer.stop();
    if (m_process.state() == QProcess::NotRunning) {
        return; // finished() or errorOccurred() is on its way
    }
    // The tool leads its own session, so its process group holds everything it started (yt-dlp's JavaScript runtime,
    // ffmpeg). Then the leader itself, in case the group signal missed it.
    const qint64 pid = m_process.processId();
    if (pid > 0) {
        ::kill(static_cast<pid_t>(-pid), SIGKILL);
    }
    m_process.kill();
}

void ToolProcess::onReadyRead(QProcess::ProcessChannel channel)
{
    if (m_done) {
        return;
    }
    if (m_spec.idleTimeout.count() > 0 && !m_killReason.has_value()) {
        m_idleTimer.start(m_spec.idleTimeout);
    }
    takeLines(channel, false);
}

void ToolProcess::takeLines(QProcess::ProcessChannel channel, bool flush)
{
    const bool out = channel == QProcess::StandardOutput;
    const QByteArray chunk = out ? m_process.readAllStandardOutput() : m_process.readAllStandardError();
    if (out && !chunk.isEmpty()) {
        if (m_outcome.standardOutput.size() + chunk.size() > m_spec.maxStdoutBytes) {
            killGroup(ToolOutcome::End::OutputTooLarge);
            return;
        }
        m_outcome.standardOutput += chunk;
        if (!m_spec.streamLines) {
            return;
        }
    }
    QByteArray &pending = out ? m_outPending : m_errPending;
    pending += chunk;
    qsizetype start = 0;
    for (qsizetype newline = pending.indexOf('\n'); newline >= 0; newline = pending.indexOf('\n', start)) {
        handleLine(channel, pending.mid(start, newline - start));
        start = newline + 1;
    }
    pending.remove(0, start);
    if (pending.size() > kMaxPendingBytes || (flush && !pending.isEmpty())) {
        handleLine(channel, std::exchange(pending, {}));
    }
}

void ToolProcess::handleLine(QProcess::ProcessChannel channel, QByteArray line)
{
    if (line.endsWith('\r')) {
        line.chop(1);
    }
    if (line.size() > kMaxLineBytes) {
        line.truncate(kMaxLineBytes);
    }
    if (channel == QProcess::StandardError) {
        m_outcome.errorLines.append(line);
        if (m_outcome.errorLines.size() > kMaxErrorLines) {
            m_outcome.errorLines.removeFirst();
        }
    }
    if (m_spec.streamLines && !m_killReason.has_value()) {
        Q_EMIT lineReady(channel, line);
    }
}

void ToolProcess::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_done) {
        return;
    }
    takeLines(QProcess::StandardOutput, true);
    takeLines(QProcess::StandardError, true);
    if (m_killReason.has_value()) {
        m_outcome.end = *m_killReason;
    } else if (status == QProcess::CrashExit) {
        m_outcome.end = ToolOutcome::End::Crashed;
    } else {
        m_outcome.end = ToolOutcome::End::Exited;
        m_outcome.exitCode = exitCode;
    }
    complete();
}

void ToolProcess::onError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || m_done) {
        return; // a crash is reported by finished()
    }
    m_outcome.end = ToolOutcome::End::FailedToStart;
    m_outcome.startError = m_process.errorString();
    complete();
}

void ToolProcess::complete()
{
    m_done = true;
    m_idleTimer.stop();
    m_totalTimer.stop();
    m_outcome.elapsed = std::chrono::milliseconds(m_elapsed.isValid() ? m_elapsed.elapsed() : 0);
    QMetaObject::invokeMethod(this, &ToolProcess::finished, Qt::QueuedConnection);
}

} // namespace selecta::meta::detail
