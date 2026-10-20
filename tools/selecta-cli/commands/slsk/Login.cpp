// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli slsk-login: log in to a local test server (Soulfind, selecta-devnet) and report what it says.
//
//   selecta-cli slsk-login --server 127.0.0.1:2242 --user alice [--password secret] [--stay 30]
#include "Command.h"
#include "commands/slsk/SlskSession.h"

using namespace Qt::StringLiterals;

namespace {

using selecta::cli::slsk::say;
using selecta::cli::slsk::SlskSession;

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    SlskSession::addOptions(parser);
    parser.addOption({u"stay"_s, u"Seconds to stay logged in, printing private messages."_s, u"seconds"_s, u"0"_s});
    if (!parser.parse(QStringList{u"slsk-login"_s} + arguments) || !parser.positionalArguments().isEmpty()) {
        say(stderr, parser.errorText().isEmpty() ? u"slsk-login takes no other arguments"_s : parser.errorText());
        return selecta::cli::kExitUsage;
    }
    SlskSession session;
    bool ok = false;
    const int stay = parser.value(u"stay"_s).toInt(&ok);
    if (!session.readOptions(parser) || !ok || stay < 0) {
        return selecta::cli::kExitUsage;
    }
    if (!session.logIn()) {
        return selecta::cli::kExitFailed;
    }
    const selecta::slsk::LoginInfo &info = session.loginInfo();
    say(stdout, u"logged in as %1; the server sees us at %2 and says: %3"_s.arg(
                    info.username, info.ownAddress.toString(), info.greeting));
    if (stay > 0) {
        selecta::slsk::ServerSession &server = session.client().server();
        QObject::connect(&server, &selecta::slsk::ServerSession::privateMessageReceived, &server,
                         [](const selecta::slsk::fromserver::PrivateMessage &message) {
                             say(stdout, u"message from %1: %2"_s.arg(message.username, message.message));
                         });
        SlskSession::waitUntil([&server] { return !server.isLoggedIn(); }, std::chrono::seconds(stay));
    }
    session.client().server().disconnectFromServer();
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"slsk-login", u"--server <host:port> --user <name> [--password <pw>] [--stay <s>]",
    u"Log in to a local Soulseek test server (never the live network) and report what it says.", &run};

} // namespace
