// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli: tools for development.
//
//   selecta-cli check-server <host>          Is this a local test server, the live network, or neither?
//   selecta-cli app <command> [key=value …]  Talk to a running Selecta started with --debug-server.
//
// Values are JSON when they parse as JSON (numbers, true/false, quoted strings), plain strings otherwise:
//   selecta-cli app navigate view=history
//   selecta-cli app screenshot path=/tmp/selecta.png name=sidebar
//   selecta-cli app tree name=toolbar depth=3
#include "DebugClient.h"
#include "base/DebugProtocol.h"
#include "base/Version.h"
#include "governor/ServerGuard.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <array>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

void print(FILE *stream, const QString &text)
{
    const QByteArray line = text.toUtf8() + '\n';
    if (std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stream) !=
        static_cast<std::size_t>(line.size())) {
        return; // stdout or stderr is gone: there is nothing else to report to
    }
}

QJsonValue parseValue(const QString &raw)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(u"[%1]"_s.arg(raw).toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isArray() && document.array().size() == 1) {
        return document.array().first();
    }
    return raw;
}

int checkServer(const QStringList &arguments)
{
    if (arguments.size() != 1) {
        print(stderr, u"usage: selecta-cli check-server <host>"_s);
        return kUsage;
    }
    using selecta::governor::ServerKind;
    const QString &host = arguments.first();
    const ServerKind kind = selecta::governor::classifyServerHost(host);
    const std::array<QString, 4> names = {u"local test server"_s, u"live Soulseek network"_s,
                                          u"host name: resolve it, then check each address"_s,
                                          u"not a server address"_s};
    const bool allowed = selecta::governor::serverAllowed(host, false);
    print(stdout, u"%1: %2; %3"_s.arg(host, names[static_cast<int>(kind)],
                                      allowed ? u"allowed"_s : u"refused by this build"_s));
    return allowed ? kOk : kFailed;
}

int app(const QStringList &arguments, const QString &socket)
{
    if (arguments.isEmpty()) {
        print(stderr,
              u"usage: selecta-cli app <command> [key=value …]\n"
              u"commands: ping, state, views, tree, screenshot, navigate, click, type, appearance, log, quit"_s);
        return kUsage;
    }
    QJsonObject args;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &pair = arguments.at(i);
        const qsizetype equals = pair.indexOf(u'=');
        if (equals <= 0) {
            print(stderr, u"arguments are key=value, got: "_s + pair);
            return kUsage;
        }
        args.insert(pair.left(equals), parseValue(pair.mid(equals + 1)));
    }

    selecta::cli::DebugClient client(socket);
    const std::optional<QJsonObject> response = client.request(arguments.first(), args);
    if (!response) {
        print(stderr, client.error());
        return kFailed;
    }
    if (!response->value("ok"_L1).toBool()) {
        print(stderr, u"error: "_s + response->value("error"_L1).toString());
        return kFailed;
    }
    const QJsonValue result = response->value("result"_L1);
    if (result.isObject() || result.isArray()) {
        const QJsonDocument document =
            result.isObject() ? QJsonDocument(result.toObject()) : QJsonDocument(result.toArray());
        print(stdout, QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed());
    } else {
        print(stdout, result.toVariant().toString());
    }
    return kOk;
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
    parser.addPositionalArgument(u"command"_s, u"check-server or app"_s);
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    parser.process(application);

    QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        print(stderr, parser.helpText());
        return kUsage;
    }
    const QString command = positional.takeFirst();
    if (command == u"check-server") {
        return checkServer(positional);
    }
    if (command == u"app") {
        return app(positional, parser.value(socket));
    }
    print(stderr, u"unknown command: "_s + command);
    return kUsage;
}
