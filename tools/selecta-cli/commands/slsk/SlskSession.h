// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "governor/Governor.h"
#include "slsk/Client.h"
#include "slsk/PermitSource.h"

#include <QCommandLineParser>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace selecta::cli::slsk {

// PeerLookup permits from the real governor, delivered on a later event-loop turn as core/ does.
class GovernorPermits final : public selecta::slsk::PermitSource
{
public:
    explicit GovernorPermits(governor::Governor &governor)
        : m_governor(governor)
    {}

    void requestPeerLookup(const QString &username, selecta::slsk::ConnectPurpose purpose,
                           Callback deliver) override; // clazy:exclude=function-args-by-ref

private:
    governor::Governor &m_governor;
    QObject m_context;
};

// What every slsk-* command shares: the options for the server, the account and the listening port; a real
// governor::Governor for permits; one slsk::Client; and a way to wait on the event loop.
//
// The server must be local (governor::ServerGuard): an IP address on this machine or the local network, or
// `localhost`. Host names are never resolved. The client listens on 127.0.0.1 only.
class SlskSession
{
public:
    SlskSession();
    ~SlskSession();
    SlskSession(const SlskSession &) = delete;
    SlskSession &operator=(const SlskSession &) = delete;
    SlskSession(SlskSession &&) = delete;
    SlskSession &operator=(SlskSession &&) = delete;

    // Adds --server, --user, --password, --listen-port, --advertise-port and --timeout to `parser`.
    static void addOptions(QCommandLineParser &parser);
    // Reads them back; on a bad value, says so on stderr and returns false.
    bool readOptions(const QCommandLineParser &parser);

    // Connects and logs in, within --timeout. Says why not on stderr.
    bool logIn();
    // What the server said at login.
    const selecta::slsk::LoginInfo &loginInfo() const { return m_info; }

    selecta::slsk::Client &client() { return *m_client; }
    governor::Governor &governor() { return m_governor; }
    std::chrono::milliseconds timeout() const { return m_timeout; }
    QString username() const { return m_user; }

    // Runs the event loop until `done` holds or `limit` passes; false on time-out.
    static bool waitUntil(const std::function<bool()> &done, std::chrono::milliseconds limit);

private:
    base::SteadyClock m_clock;
    governor::Governor m_governor{m_clock};
    GovernorPermits m_permits{m_governor};
    std::unique_ptr<selecta::slsk::Client> m_client;
    QHostAddress m_server;
    quint16 m_serverPort = 0;
    QString m_user;
    QString m_password;
    quint16 m_listenPort = 0;
    quint16 m_advertisedPort = 0;
    std::chrono::milliseconds m_timeout{std::chrono::seconds(30)};
    selecta::slsk::LoginInfo m_info;
};

// One line to stdout or stderr, flushed (other programs read these as they come).
void say(FILE *stream, const QString &text);

// "320 kbps, 4:12, VBR" and the like, for a result line.
QString describeAttributes(const selecta::slsk::FileAttributes &attributes);

} // namespace selecta::cli::slsk
