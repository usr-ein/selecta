// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

#include <chrono>
#include <optional>

namespace selecta::cli {

// Sends one request to a running Selecta's debug socket and waits for its response (base/DebugProtocol.h).
class DebugClient
{
public:
    explicit DebugClient(QString socketName)
        : m_socketName(std::move(socketName))
    {}

    // The response object, or nothing with error() set when the socket can't be reached or the reply is malformed.
    std::optional<QJsonObject> request(const QString &command, const QJsonObject &args,
                                       std::chrono::milliseconds timeout = std::chrono::seconds(15));
    QString error() const { return m_error; }

private:
    QString m_socketName;
    QString m_error;
    int m_nextId = 1;
};

} // namespace selecta::cli
