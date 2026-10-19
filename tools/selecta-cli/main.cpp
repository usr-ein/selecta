// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli: development tools. Commands register themselves (Command.h); run without arguments for the list.
// Nothing here can reach the live Soulseek network: network commands go through governor::ServerGuard.
#include "Command.h"
#include "base/DebugProtocol.h"
#include "base/Version.h"

#include <QCommandLineParser>
#include <QCoreApplication>

#include <algorithm>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

QString commandList()
{
    QString text = u"Commands:\n"_s;
    const QList<selecta::cli::Command> all = selecta::cli::commands();
    for (const selecta::cli::Command &command : all) {
        text += u"  %1 %2\n      %3\n"_s.arg(command.name, command.usage, command.summary);
    }
    return text;
}

} // namespace

int main(int argc, char *argv[])
{
    const QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(u"selecta-cli"_s);
    QCoreApplication::setApplicationVersion(QString(selecta::base::kVersion));

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Selecta development tools."_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socket(u"socket"_s, u"Debug socket of the running Selecta."_s, u"name"_s,
                                    QString(selecta::base::debugprotocol::kSocketName));
    parser.addOption(socket);
    parser.addPositionalArgument(u"command"_s, u"The command to run, then its arguments."_s);
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    parser.process(application);

    QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        selecta::cli::printLine(stderr, parser.helpText() + u'\n' + commandList());
        return selecta::cli::kExitUsage;
    }
    const QString name = positional.takeFirst();
    const QList<selecta::cli::Command> all = selecta::cli::commands();
    const auto found =
        std::ranges::find_if(all, [&](const selecta::cli::Command &command) { return command.name == name; });
    if (found == all.end()) {
        selecta::cli::printLine(stderr, u"unknown command: "_s + name + u"\n\n"_s + commandList());
        return selecta::cli::kExitUsage;
    }
    const selecta::cli::Context context{parser.value(socket)};
    return found->run(positional, context);
}
