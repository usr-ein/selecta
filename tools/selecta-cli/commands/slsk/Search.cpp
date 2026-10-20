// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli slsk-search: search a local test network and print each result as it arrives, one per line:
// user, free slot, speed, queue, size, attributes, path (tab-separated).
//
//   selecta-cli slsk-search --server 127.0.0.1:2242 --user alice [--wait 10] kel varo
#include "Command.h"
#include "commands/slsk/SlskSession.h"

#include <QSet>

using namespace Qt::StringLiterals;

namespace {

using selecta::cli::slsk::say;
using selecta::cli::slsk::SlskSession;

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    SlskSession::addOptions(parser);
    parser.addOption({u"wait"_s, u"Seconds to collect results."_s, u"seconds"_s, u"10"_s});
    parser.addPositionalArgument(u"query"_s, u"What to search for."_s, u"<query…>"_s);
    if (!parser.parse(QStringList{u"slsk-search"_s} + arguments) || parser.positionalArguments().isEmpty()) {
        say(stderr,
            parser.errorText().isEmpty() ? u"usage: selecta-cli slsk-search [options] <query…>"_s : parser.errorText());
        return selecta::cli::kExitUsage;
    }
    SlskSession session;
    bool ok = false;
    const int wait = parser.value(u"wait"_s).toInt(&ok);
    if (!session.readOptions(parser) || !ok || wait <= 0 || wait > 600) {
        return selecta::cli::kExitUsage;
    }
    if (!session.logIn()) {
        return selecta::cli::kExitFailed;
    }
    selecta::governor::Request request;
    request.query = parser.positionalArguments().join(u' ');
    selecta::governor::Decision decision = session.governor().request(selecta::governor::Operation::Search, request);
    if (!decision.permit) {
        say(stderr, u"governor: no search now: "_s + decision.explanation);
        return selecta::cli::kExitFailed;
    }
    selecta::slsk::Client &client = session.client();
    struct Tally
    {
        int files = 0;
        QSet<QString> users;
    } tally;
    const QObject receiver; // goes before `tally`
    QObject::connect(
        &client.peers(), &selecta::slsk::PeerNetwork::searchResponseReceived, &receiver,
        [counts = &tally](const QString &user, const selecta::slsk::peer::FileSearchResponse &response) {
            counts->users.insert(user);
            for (const selecta::slsk::FileEntry &file : response.results) {
                ++counts->files;
                say(stdout, u"%1\t%2\t%3\t%4\t%5\t%6\t%7"_s.arg(
                                user, response.slotFree ? u"free"_s : u"busy"_s, QString::number(response.averageSpeed),
                                QString::number(response.queueLength), QString::number(file.size),
                                selecta::cli::slsk::describeAttributes(file.attributes), file.name.display()));
            }
        });
    const quint32 token = client.search(decision.cleanedQuery, std::move(*decision.permit));
    if (token == 0) {
        say(stderr, u"the search couldn't be sent"_s);
        return selecta::cli::kExitFailed;
    }
    SlskSession::waitUntil([] { return false; }, std::chrono::seconds(wait));
    client.stopSearch(token);
    say(stderr,
        u"%1 files from %2 users for \"%3\""_s.arg(tally.files).arg(tally.users.size()).arg(decision.cleanedQuery));
    client.server().disconnectFromServer();
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"slsk-search", u"--server <host:port> --user <name> [--wait <s>] <query…>",
    u"Search a local Soulseek test network and print the results (tab-separated).", &run};

} // namespace
