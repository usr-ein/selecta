// SPDX-License-Identifier: GPL-3.0-or-later
#include "DebugClient.h"

#include "base/DebugProtocol.h"

#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QLocalSocket>

using namespace Qt::StringLiterals;

namespace selecta::cli {

std::optional<QJsonObject> DebugClient::request(const QString &command, const QJsonObject &args,
                                                std::chrono::milliseconds timeout)
{
    m_error.clear();
    const QDeadlineTimer deadline(timeout);
    QLocalSocket socket;
    socket.connectToServer(m_socketName);
    if (!socket.waitForConnected(static_cast<int>(deadline.remainingTime()))) {
        m_error = u"can't reach Selecta on \"%1\" (%2). Start it with --debug-server."_s.arg(m_socketName,
                                                                                             socket.errorString());
        return std::nullopt;
    }

    const int id = m_nextId++;
    const QJsonObject message{{"id"_L1, id}, {"cmd"_L1, command}, {"args"_L1, args}};
    socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(static_cast<int>(deadline.remainingTime()))) {
        m_error = u"sending failed: "_s + socket.errorString();
        return std::nullopt;
    }

    QByteArray buffer;
    while (!buffer.contains('\n')) {
        if (deadline.hasExpired() || !socket.waitForReadyRead(static_cast<int>(deadline.remainingTime()))) {
            m_error = deadline.hasExpired() ? u"no answer within %1 ms"_s.arg(timeout.count())
                                            : u"connection closed: "_s + socket.errorString();
            return std::nullopt;
        }
        buffer += socket.readAll();
        if (buffer.size() > base::debugprotocol::kMaxLineBytes * 64) {
            m_error = u"answer too large"_s;
            return std::nullopt;
        }
    }
    socket.disconnectFromServer();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n')), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_error = u"malformed answer: "_s + parseError.errorString();
        return std::nullopt;
    }
    return document.object();
}

} // namespace selecta::cli
