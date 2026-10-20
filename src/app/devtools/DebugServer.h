// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QPointer>

class QLocalSocket;

namespace selecta::app {

class MainWindow;

namespace devtools {

// A local control socket for inspecting and driving a running Selecta: state, widget tree, screenshots, navigation,
// clicks, typing, row selection, the player and the log (protocol: base/DebugProtocol.h; client: `selecta-cli app …`).
//
// Built only with SELECTA_DEVTOOLS and started only with --debug-server. The socket is reachable by the same user
// only. Everything runs on the main thread, where the widgets live.
class DebugServer : public QObject
{
    Q_OBJECT

public:
    DebugServer(MainWindow &window, QString logDir, QObject *parent = nullptr);
    ~DebugServer() override;

    // Listens on `name` (a stale socket left by a crash is removed first). Returns false if that fails.
    bool listen(const QString &name);
    QString fullServerName() const { return m_server.fullServerName(); }

    // Handles one request object and returns the response object. Public for tests.
    QJsonObject handle(const QJsonObject &request);

private:
    void acceptConnections();
    void readFrom(QLocalSocket *socket);

    QJsonValue ping();
    QJsonValue state();
    QJsonValue tree(const QJsonObject &args, QString *error);
    QJsonValue screenshot(const QJsonObject &args, QString *error);
    QJsonValue navigate(const QJsonObject &args, QString *error);
    QJsonValue click(const QJsonObject &args, QString *error);
    QJsonValue type(const QJsonObject &args, QString *error);
    QJsonValue appearance(const QJsonObject &args, QString *error);
    QJsonValue log(const QJsonObject &args, QString *error);
    QJsonValue select(const QJsonObject &args, QString *error);
    QJsonValue player(const QJsonObject &args, QString *error);

    MainWindow &m_window;
    QString m_logDir;
    QLocalServer m_server;
    QHash<QLocalSocket *, QByteArray> m_buffers;
};

} // namespace devtools
} // namespace selecta::app
