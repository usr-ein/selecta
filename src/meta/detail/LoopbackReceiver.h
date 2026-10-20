// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Result.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <chrono>

class QTcpServer;
class QTcpSocket;

namespace selecta::meta::detail {

// The one-shot HTTP listener that receives the browser's answer to a Spotify sign-in (RFC 8252 loopback redirect):
// http://127.0.0.1:<port>/callback?code=…&state=… (or ?error=…&state=…).
//
// - Binds 127.0.0.1 only; refuses peers that aren't loopback.
// - Handles several connections at once (browsers open speculative ones that never send anything), each bounded in
//   size (8 KiB of request head) and time (10 s).
// - Answers other paths (/favicon.ico) with 404 and keeps waiting; answers non-GET methods with 405.
// - The first request to /callback decides: `state` must equal the expected one (compared in constant time), then
//   `error=access_denied` → AccessDenied, another error → BadResponse, a code → codeReceived(). Either way the browser
//   gets a small static page (nothing from the request is echoed back) and the listener closes.
//
// Threading: the owner's thread. Emits exactly one of codeReceived() / failed() at most once.
class LoopbackReceiver final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 kMaxRequestBytes = qint64{8} * 1024;
    static constexpr int kMaxConnections = 16;
    static constexpr int kMaxCodeLength = 2048;
    static constexpr std::chrono::seconds kConnectionTimeout{10};

    explicit LoopbackReceiver(QByteArray expectedState, QObject *parent = nullptr);
    ~LoopbackReceiver() override;
    LoopbackReceiver(const LoopbackReceiver &) = delete;
    LoopbackReceiver &operator=(const LoopbackReceiver &) = delete;
    LoopbackReceiver(LoopbackReceiver &&) = delete;
    LoopbackReceiver &operator=(LoopbackReceiver &&) = delete;

    // Starts listening on 127.0.0.1:`port` (0: a free port the system picks). PortUnavailable when it can't.
    Result<void> listen(quint16 port);
    bool isListening() const;
    quint16 port() const;
    QUrl redirectUri() const; // http://127.0.0.1:<port>/callback

    // Stops listening and drops every connection, without emitting anything.
    void close();

Q_SIGNALS:
    void codeReceived(const QByteArray &code);
    void failed(const selecta::meta::MetaError &error);

private:
    struct Connection
    {
        QByteArray buffer;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket, const QByteArray &head);
    void respond(QTcpSocket *socket, int status, const QByteArray &reason, const QString &title, const QString &text);
    void finish(const QByteArray &code, const std::optional<MetaError> &error);

    QByteArray m_expectedState;
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, Connection> m_connections;
    bool m_done = false;
};

} // namespace selecta::meta::detail
