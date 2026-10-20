// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/slsk/SlskSession.h"

#include "governor/ServerGuard.h"
#include "slsk/PeerMessages.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QMetaEnum>
#include <QTimer>

#include <cstdio>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::cli::slsk {
namespace {

std::optional<quint16> portValue(const QString &text, bool allowZero)
{
    bool ok = false;
    const uint value = text.toUInt(&ok);
    if (!ok || value > 65535 || (value == 0 && !allowZero)) {
        return std::nullopt;
    }
    return static_cast<quint16>(value);
}

} // namespace

void GovernorPermits::requestPeerLookup(const QString &username, selecta::slsk::ConnectPurpose /*purpose*/,
                                        Callback deliver) // clazy:exclude=function-args-by-ref
{
    governor::Request request;
    request.peer = username;
    governor::Decision decision = m_governor.request(governor::Operation::PeerLookup, request);
    if (!decision.permit) {
        say(stderr, u"governor: no connection to %1 now: %2"_s.arg(username, decision.explanation));
    }
    // A permit is move-only: it travels in a shared holder to the later turn.
    const auto permit = std::make_shared<std::optional<governor::Permit>>(std::move(decision.permit));
    QTimer::singleShot(0, &m_context, [deliver, permit] { deliver(std::move(*permit)); });
}

SlskSession::SlskSession()
    : m_client(std::make_unique<selecta::slsk::Client>(m_permits))
{}

SlskSession::~SlskSession() = default;

void SlskSession::addOptions(QCommandLineParser &parser)
{
    parser.addOption({u"server"_s, u"The test server as host:port, on this machine or the local network."_s,
                      u"host:port"_s, u"127.0.0.1:2242"_s});
    parser.addOption({u"user"_s, u"The account (a test server registers it at the first login)."_s, u"name"_s});
    parser.addOption({u"password"_s, u"The account's password (default: the name)."_s, u"password"_s});
    parser.addOption(
        {u"listen-port"_s, u"The port to listen on, on 127.0.0.1 (default: any free one)."_s, u"port"_s, u"0"_s});
    parser.addOption({u"advertise-port"_s,
                      u"The port to tell the server instead: peers then reach us only through it (indirect)."_s,
                      u"port"_s});
    parser.addOption({u"timeout"_s, u"Seconds to wait for each step."_s, u"seconds"_s, u"30"_s});
}

bool SlskSession::readOptions(const QCommandLineParser &parser)
{
    const QString server = parser.value(u"server"_s);
    const qsizetype colon = server.lastIndexOf(u':');
    const QString host = colon > 0 ? server.left(colon) : QString();
    const std::optional<quint16> port = colon > 0 ? portValue(server.mid(colon + 1), false) : std::nullopt;
    if (host.isEmpty() || !port) {
        say(stderr, u"--server takes host:port, e.g. 127.0.0.1:2242"_s);
        return false;
    }
    if (!governor::serverAllowed(host, false)) {
        switch (governor::classifyServerHost(host)) {
        case governor::ServerKind::Live:
            say(stderr,
                u"refusing %1: that is the live Soulseek network; selecta-cli only talks to test servers"_s.arg(host));
            break;
        case governor::ServerKind::Unresolved:
            say(stderr, u"give the server as an IP address: selecta-cli never resolves host names"_s);
            break;
        default:
            say(stderr, u"%1 is not a server address"_s.arg(host));
            break;
        }
        return false;
    }
    m_server = host == u"localhost"_s ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(host);
    m_serverPort = *port;
    m_user = parser.value(u"user"_s);
    if (!selecta::slsk::isAcceptablePeerUsername(m_user)) {
        say(stderr, u"--user takes a Soulseek username (printable, at most 256 bytes)"_s);
        return false;
    }
    m_password = parser.isSet(u"password"_s) ? parser.value(u"password"_s) : m_user;
    const std::optional<quint16> listen = portValue(parser.value(u"listen-port"_s), true);
    const std::optional<quint16> advertised =
        parser.isSet(u"advertise-port"_s) ? portValue(parser.value(u"advertise-port"_s), false) : quint16{0};
    bool ok = false;
    const int seconds = parser.value(u"timeout"_s).toInt(&ok);
    if (!listen || !advertised || !ok || seconds <= 0 || seconds > 3600) {
        say(stderr, u"--listen-port and --advertise-port take a port, --timeout 1 to 3600 seconds"_s);
        return false;
    }
    m_listenPort = *listen;
    m_advertisedPort = *advertised;
    m_timeout = std::chrono::seconds(seconds);
    return true;
}

bool SlskSession::logIn()
{
    governor::Request request;
    request.server = m_server;
    request.liveEnabledByUser = false;
    governor::Decision decision = m_governor.request(governor::Operation::Login, request);
    if (!decision.permit) {
        say(stderr, u"governor: no login now: "_s + decision.explanation);
        return false;
    }
    selecta::slsk::Client &c = *m_client;
    QString error;
    if (!c.peers().listen(m_listenPort, QHostAddress(QHostAddress::LocalHost), &error)) {
        say(stderr, u"can't listen on 127.0.0.1:%1: %2"_s.arg(m_listenPort).arg(error));
        return false;
    }
    c.server().setListenPort(m_advertisedPort != 0 ? m_advertisedPort : c.peers().listenPort());
    std::optional<selecta::slsk::ServerSession::EndReason> ended;
    QString detail;
    const QMetaObject::Connection watch = QObject::connect(
        &c.server(), &selecta::slsk::ServerSession::ended, &c.server(),
        [endedAt = &ended, detailAt = &detail](selecta::slsk::ServerSession::EndReason reason, const QString &text) {
            *endedAt = reason;
            *detailAt = text;
        });
    const QMetaObject::Connection info =
        QObject::connect(&c.server(), &selecta::slsk::ServerSession::loggedIn, &c.server(),
                         [this](const selecta::slsk::LoginInfo &loginInfo) { m_info = loginInfo; });
    const bool started = c.server().connectToServer(m_server, m_serverPort, std::move(*decision.permit)) &&
                         c.server().login(m_user, m_password);
    const bool done = started && waitUntil([&] { return c.server().isLoggedIn() || ended.has_value(); }, m_timeout);
    QObject::disconnect(watch);
    QObject::disconnect(info);
    if (c.server().isLoggedIn()) {
        m_governor.serverEvent(governor::ReconnectPolicy::Event::LoginSucceeded);
        return true;
    }
    if (!started) {
        say(stderr, u"couldn't start the login"_s);
    } else if (!done || !ended) {
        say(stderr, u"no login reply within %1 s"_s.arg(m_timeout.count() / 1000));
    } else {
        const char *name =
            QMetaEnum::fromType<selecta::slsk::ServerSession::EndReason>().valueToKey(static_cast<int>(*ended));
        say(stderr,
            u"login failed: %1 %2 %3"_s.arg(QString::fromLatin1(name), c.server().loginRejection().reason, detail));
    }
    return false;
}

bool SlskSession::waitUntil(const std::function<bool()> &done, std::chrono::milliseconds limit)
{
    const QDeadlineTimer deadline(limit);
    while (!done()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    }
    return true;
}

void say(FILE *stream, const QString &text)
{
    const QByteArray line = text.toUtf8() + '\n';
    if (std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stream) !=
            static_cast<std::size_t>(line.size()) ||
        std::fflush(stream) != 0) {
        return; // stdout or stderr is gone: there is nowhere else to report it
    }
}

QString describeAttributes(const selecta::slsk::FileAttributes &attributes)
{
    QStringList parts;
    if (attributes.bitrate) {
        parts.append(u"%1 kbps"_s.arg(*attributes.bitrate));
    }
    if (attributes.vbr == true) {
        parts.append(u"VBR"_s);
    }
    if (attributes.sampleRate) {
        parts.append(u"%1 Hz"_s.arg(*attributes.sampleRate));
    }
    if (attributes.bitDepth) {
        parts.append(u"%1 bit"_s.arg(*attributes.bitDepth));
    }
    if (attributes.duration) {
        parts.append(u"%1:%2"_s.arg(*attributes.duration / 60).arg(*attributes.duration % 60, 2, 10, QChar(u'0')));
    }
    return parts.join(u", "_s);
}

} // namespace selecta::cli::slsk
