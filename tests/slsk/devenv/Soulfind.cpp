// SPDX-License-Identifier: GPL-3.0-or-later
#include "devenv/Soulfind.h"

#include "fixtures/TestUtil.h"

#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::slsk::testing {

quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

QString soulfindBinary()
{
    if (const QString path = qEnvironmentVariable("SELECTA_SOULFIND"); !path.isEmpty()) {
        return path;
    }
    const QString cached = QStringLiteral(SELECTA_DEVENV_DIR) + u"/.cache/soulfind/current/soulfind"_s;
    return QFileInfo(cached).isExecutable() ? cached : QString();
}

bool devenvEnabled()
{
    return qEnvironmentVariable("SELECTA_DEVENV") == u"1"_s;
}

Soulfind::~Soulfind()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(3000)) {
            m_process.kill();
            m_process.waitForFinished(3000);
        }
    }
    if (qEnvironmentVariable("SELECTA_SOULFIND_DEBUG") == u"1"_s) {
        qInfo().noquote() << "--- Soulfind's output ---\n" << QString::fromLocal8Bit(m_process.readAll());
    }
}

QString Soulfind::start(const QString &binary)
{
    m_port = freePort();
    if (m_port == 0 || !m_data.isValid()) {
        return u"no free port or temporary folder"_s;
    }
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_MACOS
    // As devenv/run-soulfind.sh: Soulfind needs Homebrew's SQLite on macOS.
    for (const QString &lib : {u"/opt/homebrew/opt/sqlite/lib"_s, u"/usr/local/opt/sqlite/lib"_s}) {
        if (QFileInfo(lib).isDir()) {
            environment.insert(u"DYLD_LIBRARY_PATH"_s, lib);
            break;
        }
    }
#endif
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    QStringList arguments{u"--port"_s, QString::number(m_port), u"--database"_s, m_data.filePath(u"soulfind.db"_s)};
    if (qEnvironmentVariable("SELECTA_SOULFIND_DEBUG") == u"1"_s) {
        arguments.append(u"--debug"_s);
    }
    m_process.start(binary, arguments);
    if (!m_process.waitForStarted(5000)) {
        return u"couldn't start %1: %2"_s.arg(binary, m_process.errorString());
    }
    const bool up = waitFor(
        [this] {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, m_port);
            return probe.waitForConnected(200);
        },
        10s);
    if (!up) {
        return u"Soulfind didn't listen: %1"_s.arg(QString::fromLocal8Bit(m_process.readAll()));
    }
    return {};
}

} // namespace selecta::slsk::testing
