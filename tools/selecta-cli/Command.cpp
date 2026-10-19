// SPDX-License-Identifier: GPL-3.0-or-later
#include "Command.h"

#include <QByteArray>

#include <algorithm>
#include <cstdio>

namespace selecta::cli {
namespace {

// Registration happens during static initialisation, before main(); a function-local static is initialised on first
// use, so the order of translation units doesn't matter.
QList<Command> &registry()
{
    static QList<Command> commands;
    return commands;
}

} // namespace

CommandRegistration::CommandRegistration(const char16_t *name, const char16_t *usage, const char16_t *summary,
                                         int (*run)(const QStringList &, const Context &)) noexcept
{
    registry().append(Command{QString::fromUtf16(name), QString::fromUtf16(usage), QString::fromUtf16(summary), run});
}

QList<Command> commands()
{
    QList<Command> sorted = registry();
    std::ranges::sort(sorted, [](const Command &a, const Command &b) { return a.name < b.name; });
    return sorted;
}

void printLine(FILE *stream, const QString &text)
{
    const QByteArray line = text.toUtf8() + '\n';
    if (std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stream) !=
        static_cast<std::size_t>(line.size())) {
        return; // stdout or stderr is gone: there is nowhere else to report it
    }
}

} // namespace selecta::cli
