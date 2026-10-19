// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace selecta::cli {

// Options shared by every command, parsed by main.cpp.
struct Context
{
    QString debugSocket; // --socket: the running Selecta's debug socket (app commands)
};

struct Command
{
    QString name;    // what the user types, e.g. "slsk-login"
    QString usage;   // the arguments, e.g. "--server 127.0.0.1:2242 --user <name>"
    QString summary; // one line: what it does
    int (*run)(const QStringList &arguments, const Context &context) = nullptr; // returns the exit code
};

// Each command registers itself from its own source file, so libraries add commands without touching shared files:
//
//   namespace {
//   const selecta::cli::CommandRegistration registration{u"name", u"<args>", u"What it does.", &run};
//   }
//
// Files under tools/selecta-cli/commands/<library>/ are compiled when that library is part of the build.
class CommandRegistration
{
public:
    // Takes only literals and a function pointer, so nothing can throw before the constructor runs; running out of
    // memory while registering at start-up then terminates, which is the only sensible outcome.
    CommandRegistration(const char16_t *name, const char16_t *usage, const char16_t *summary,
                        int (*run)(const QStringList &, const Context &)) noexcept;
};

// All registered commands, sorted by name.
QList<Command> commands();

// Exit codes shared by commands.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailed = 1;
inline constexpr int kExitUsage = 2;

// Writes one line to stdout or stderr. Output errors are ignored: there is nowhere else to report them.
void printLine(FILE *stream, const QString &text);

} // namespace selecta::cli
