// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli slsk-browse: list what a user on a local test network shares: each folder, then its files (size,
// attributes, name), tab-separated.
//
//   selecta-cli slsk-browse --server 127.0.0.1:2242 --user alice bob
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
    parser.addPositionalArgument(u"peer"_s, u"Whose shares to list."_s, u"<peer>"_s);
    if (!parser.parse(QStringList{u"slsk-browse"_s} + arguments) || parser.positionalArguments().size() != 1) {
        say(stderr,
            parser.errorText().isEmpty() ? u"usage: selecta-cli slsk-browse [options] <peer>"_s : parser.errorText());
        return selecta::cli::kExitUsage;
    }
    SlskSession session;
    if (!session.readOptions(parser)) {
        return selecta::cli::kExitUsage;
    }
    const QString peer = parser.positionalArguments().constFirst();
    if (!session.logIn()) {
        return selecta::cli::kExitFailed;
    }
    selecta::governor::Request request;
    request.peer = peer;
    selecta::governor::Decision decision = session.governor().request(selecta::governor::Operation::Browse, request);
    if (!decision.permit) {
        say(stderr, u"governor: no browse now: "_s + decision.explanation);
        return selecta::cli::kExitFailed;
    }
    selecta::slsk::PeerNetwork &peers = session.client().peers();
    std::optional<selecta::slsk::SharedFileList> list;
    std::optional<selecta::slsk::PeerError> failure;
    const QObject receiver; // goes before `list` and `failure`
    QObject::connect(&peers, &selecta::slsk::PeerNetwork::sharedFileListReceived, &receiver,
                     [peer, listAt = &list](const QString &user, const selecta::slsk::SharedFileList &shares) {
                         if (user == peer) {
                             *listAt = shares;
                         }
                     });
    QObject::connect(&peers, &selecta::slsk::PeerNetwork::requestFailed, &receiver,
                     [peer, failureAt = &failure](const QString &user, selecta::slsk::PeerRequest kind,
                                                  quint32 /*token*/, selecta::slsk::PeerError error) {
                         if (user == peer && kind == selecta::slsk::PeerRequest::SharedFileList) {
                             *failureAt = error;
                         }
                     });
    if (!peers.requestSharedFileList(peer, std::move(*decision.permit))) {
        say(stderr, u"the browse couldn't be sent"_s);
        return selecta::cli::kExitFailed;
    }
    SlskSession::waitUntil([&] { return list.has_value() || failure.has_value(); }, session.timeout());
    if (!list) {
        const QString why = failure ? QString::fromLatin1(selecta::slsk::peerErrorName(*failure)) : u"timed out"_s;
        say(stderr, u"no share list from %1: %2"_s.arg(peer, why));
        return selecta::cli::kExitFailed;
    }
    int files = 0;
    for (const selecta::slsk::SharedDirectory &directory : std::as_const(list->directories)) {
        say(stdout, directory.path.display());
        for (const selecta::slsk::FileEntry &file : directory.files) {
            ++files;
            say(stdout,
                u"\t%1\t%2\t%3"_s.arg(QString::number(file.size),
                                      selecta::cli::slsk::describeAttributes(file.attributes), file.name.display()));
        }
    }
    say(stderr, u"%1 shares %2 files in %3 folders"_s.arg(peer).arg(files).arg(list->directories.size()));
    session.client().server().disconnectFromServer();
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{u"slsk-browse", u"--server <host:port> --user <name> <peer>",
                                                     u"List what a user on a local Soulseek test network shares.",
                                                     &run};

} // namespace
