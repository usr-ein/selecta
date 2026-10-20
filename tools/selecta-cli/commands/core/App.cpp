// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli app <command> [key=value …]: talk to a running Selecta started with --debug-server.
//
// Values are JSON when they parse as JSON (numbers, true/false, quoted strings), plain strings otherwise:
//   selecta-cli app navigate view=history
//   selecta-cli app screenshot path=/tmp/selecta.png name=sidebar
//   selecta-cli app tree name=toolbar depth=3
#include "Command.h"
#include "DebugClient.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

QJsonValue parseValue(const QString &raw)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(u"[%1]"_s.arg(raw).toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isArray() && document.array().size() == 1) {
        return document.array().first();
    }
    return raw;
}

int run(const QStringList &arguments, const selecta::cli::Context &context)
{
    using selecta::cli::printLine;
    if (arguments.isEmpty()) {
        printLine(stderr,
                  u"usage: selecta-cli app <command> [key=value …]\n"
                  u"commands: ping, state, views, tree, screenshot, navigate, click, type, appearance, log, select, "
                  u"play-pause, player, notifications, quit"_s);
        return selecta::cli::kExitUsage;
    }
    QJsonObject args;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &pair = arguments.at(i);
        const qsizetype equals = pair.indexOf(u'=');
        if (equals <= 0) {
            printLine(stderr, u"arguments are key=value, got: "_s + pair);
            return selecta::cli::kExitUsage;
        }
        args.insert(pair.left(equals), parseValue(pair.mid(equals + 1)));
    }

    selecta::cli::DebugClient client(context.debugSocket);
    const std::optional<QJsonObject> response = client.request(arguments.first(), args);
    if (!response) {
        printLine(stderr, client.error());
        return selecta::cli::kExitFailed;
    }
    if (!response->value("ok"_L1).toBool()) {
        printLine(stderr, u"error: "_s + response->value("error"_L1).toString());
        return selecta::cli::kExitFailed;
    }
    const QJsonValue result = response->value("result"_L1);
    if (result.isObject() || result.isArray()) {
        const QJsonDocument document =
            result.isObject() ? QJsonDocument(result.toObject()) : QJsonDocument(result.toArray());
        printLine(stdout, QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed());
    } else {
        printLine(stdout, result.toVariant().toString());
    }
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"app", u"<command> [key=value …]",
    u"Inspect or drive a running Selecta started with --debug-server (see tools/dev/app.sh).", &run};

} // namespace
