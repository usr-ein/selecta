// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli check-server <host>: is this a local test server, the live network, or neither?
#include "Command.h"
#include "governor/ServerGuard.h"

#include <array>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    using selecta::cli::printLine;
    if (arguments.size() != 1) {
        printLine(stderr, u"usage: selecta-cli check-server <host>"_s);
        return selecta::cli::kExitUsage;
    }
    using selecta::governor::ServerKind;
    const QString &host = arguments.first();
    const ServerKind kind = selecta::governor::classifyServerHost(host);
    const std::array<QString, 4> names = {u"local test server"_s, u"live Soulseek network"_s,
                                          u"host name: resolve it, then check each address"_s,
                                          u"not a server address"_s};
    const bool allowed = selecta::governor::serverAllowed(host, false);
    printLine(stdout, u"%1: %2; %3"_s.arg(host, names.at(static_cast<std::size_t>(kind)),
                                          allowed ? u"allowed"_s : u"refused by this build"_s));
    return allowed ? selecta::cli::kExitOk : selecta::cli::kExitFailed;
}

const selecta::cli::CommandRegistration registration{
    u"check-server", u"<host>", u"Classify a server address: local test server, live network or invalid.", &run};

} // namespace
