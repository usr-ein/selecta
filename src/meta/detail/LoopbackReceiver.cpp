// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/LoopbackReceiver.h"

#include "meta/Pkce.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <optional>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail {

namespace {

QString htmlEscaped(const QString &text)
{
    return text.toHtmlEscaped();
}

} // namespace

LoopbackReceiver::LoopbackReceiver(QByteArray expectedState, QObject *parent)
    : QObject(parent)
    , m_expectedState(std::move(expectedState))
    , m_server(new QTcpServer(this))
{
    m_server->setMaxPendingConnections(kMaxConnections);
    connect(m_server, &QTcpServer::newConnection, this, &LoopbackReceiver::onNewConnection);
}

LoopbackReceiver::~LoopbackReceiver()
{
    close();
}

Result<void> LoopbackReceiver::listen(quint16 port)
{
    if (m_server->isListening()) {
        return {};
    }
    if (!m_server->listen(QHostAddress(QHostAddress::LocalHost), port)) {
        return makeError(Service::SpotifyAccounts, ErrorCode::PortUnavailable,
                         u"Selecta couldn't listen on 127.0.0.1:%1 for the sign-in answer (%2)."_s.arg(port).arg(
                             m_server->errorString()));
    }
    return {};
}

bool LoopbackReceiver::isListening() const
{
    return m_server->isListening();
}

quint16 LoopbackReceiver::port() const
{
    return m_server->serverPort();
}

QUrl LoopbackReceiver::redirectUri() const
{
    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"127.0.0.1"_s);
    url.setPort(port());
    url.setPath(u"/callback"_s);
    return url;
}

void LoopbackReceiver::close()
{
    m_server->close();
    const QList<QTcpSocket *> sockets = m_connections.keys();
    m_connections.clear();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

void LoopbackReceiver::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (m_done || m_connections.size() >= kMaxConnections || !socket->peerAddress().isLoopback()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_connections.insert(socket, Connection{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_connections.remove(socket);
            socket->deleteLater();
        });
        // A connection that sends nothing (a browser's speculative one) or dribbles is dropped.
        QTimer::singleShot(kConnectionTimeout, socket, [this, socket] {
            if (m_connections.remove(socket) > 0) {
                socket->abort();
                socket->deleteLater();
            }
        });
    }
}

void LoopbackReceiver::onReadyRead(QTcpSocket *socket)
{
    const auto it = m_connections.find(socket);
    if (it == m_connections.end()) {
        return;
    }
    it->buffer += socket->read(kMaxRequestBytes + 1 - it->buffer.size());
    const qsizetype end = it->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (it->buffer.size() > kMaxRequestBytes) {
            m_connections.erase(it);
            respond(socket, 431, "Request Header Fields Too Large", u"Request too large"_s,
                    u"Selecta couldn't read this request."_s);
        }
        return;
    }
    const QByteArray head = it->buffer.left(end);
    m_connections.erase(it); // one request per connection
    handleRequest(socket, head);
}

void LoopbackReceiver::handleRequest(QTcpSocket *socket, const QByteArray &head)
{
    const QByteArray requestLine = head.left(head.indexOf("\r\n") >= 0 ? head.indexOf("\r\n") : head.size());
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() != 3 || !parts.at(2).startsWith("HTTP/1.")) {
        respond(socket, 400, "Bad Request", u"Bad request"_s, u"Selecta couldn't read this request."_s);
        return;
    }
    if (parts.at(0) != "GET") {
        respond(socket, 405, "Method Not Allowed", u"Not allowed"_s, u"Selecta only answers the sign-in page."_s);
        return;
    }
    const QUrl target = QUrl::fromEncoded(parts.at(1));
    if (!target.isValid() || target.path() != u"/callback"_s) {
        respond(socket, 404, "Not Found", u"Not found"_s, u"There's nothing here."_s);
        return;
    }
    if (m_done) {
        respond(socket, 409, "Conflict", u"Already done"_s,
                u"Selecta already received an answer for this sign-in. You can close this tab."_s);
        return;
    }

    const QUrlQuery query(target);
    const QByteArray state = query.queryItemValue(u"state"_s, QUrl::FullyDecoded).toUtf8();
    if (state.isEmpty() || !pkce::constantTimeEquals(state, m_expectedState)) {
        respond(socket, 400, "Bad Request", u"Sign-in not recognised"_s,
                u"This answer doesn't belong to the sign-in Selecta started. Close this tab and start again from "
                u"Selecta."_s);
        finish({}, makeError(Service::SpotifyAccounts, ErrorCode::StateMismatch,
                             u"The browser's answer doesn't match this sign-in (state mismatch)."_s));
        return;
    }
    if (query.hasQueryItem(u"error"_s)) {
        const QString reason = query.queryItemValue(u"error"_s, QUrl::FullyDecoded);
        if (reason == u"access_denied"_s) {
            respond(socket, 200, "OK", u"Sign-in cancelled"_s,
                    u"You didn't allow Selecta to use your Spotify account. You can close this tab."_s);
            finish({}, makeError(Service::SpotifyAccounts, ErrorCode::AccessDenied,
                                 u"Access to Spotify was declined in the browser."_s));
            return;
        }
        respond(socket, 200, "OK", u"Sign-in failed"_s,
                u"Spotify reported a problem with this sign-in. Close this tab and try again from Selecta."_s);
        // Only a short, plain-ASCII form of Spotify's reason goes into the message.
        QString shortReason;
        const QString cut = reason.left(64);
        for (const QChar c : cut) {
            if (c.isLetterOrNumber() || c == u'_' || c == u'-') {
                shortReason += c;
            }
        }
        finish({}, makeError(Service::SpotifyAccounts, ErrorCode::BadResponse,
                             u"Spotify refused the sign-in (%1)."_s.arg(shortReason)));
        return;
    }
    const QByteArray code = query.queryItemValue(u"code"_s, QUrl::FullyDecoded).toUtf8();
    if (code.isEmpty() || code.size() > kMaxCodeLength) {
        respond(socket, 400, "Bad Request", u"Sign-in failed"_s,
                u"Selecta couldn't read Spotify's answer. Close this tab and try again from Selecta."_s);
        finish({}, makeError(Service::SpotifyAccounts, ErrorCode::BadResponse,
                             u"The browser's answer carries no usable authorisation code."_s));
        return;
    }
    respond(socket, 200, "OK", u"Signed in"_s,
            u"Selecta received your Spotify sign-in. You can close this tab and go back to Selecta."_s);
    finish(code, std::nullopt);
}

void LoopbackReceiver::respond(QTcpSocket *socket, int status, const QByteArray &reason, const QString &title,
                               const QString &text)
{
    const QByteArray body =
        (u"<!doctype html><html><head><meta charset=\"utf-8\"><title>Selecta — %1</title>"
         u"<style>body{font:15px -apple-system,system-ui,sans-serif;margin:15vh auto;max-width:32em;padding:0 1em}"
         u"</style></head><body><h1>%1</h1><p>%2</p></body></html>"_s.arg(htmlEscaped(title), htmlEscaped(text)))
            .toUtf8();
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    response += "Content-Type: text/html; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Referrer-Policy: no-referrer\r\n";
    response += "X-Content-Type-Options: nosniff\r\n";
    response += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost(); // after the bytes are written; `disconnected` then deletes it
}

void LoopbackReceiver::finish(const QByteArray &code, const std::optional<MetaError> &error)
{
    if (m_done) {
        return;
    }
    m_done = true;
    m_server->close(); // no new connections; the answering socket finishes writing on its own
    if (error.has_value()) {
        Q_EMIT failed(*error);
    } else {
        Q_EMIT codeReceived(code);
    }
}

} // namespace selecta::meta::detail
