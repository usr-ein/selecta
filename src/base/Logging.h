// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLoggingCategory>
#include <QString>

// Logging categories are named selecta.<module>[.<sub>]. Each module declares its own; these are the shared ones.
Q_DECLARE_LOGGING_CATEGORY(lcBase)

namespace selecta::base {

// Copies every Qt log message, from any thread, into a rotating file in `dir` (selecta.log, selecta.1.log, …), then
// passes it on to the previous handler (stderr, or os_log on macOS). Keeps at most `keepFiles` files of about
// `maxBytes` each. Calling it again re-targets the file. Returns false when the file can't be opened; messages still
// reach the previous handler.
bool installFileLogger(const QString &dir, qint64 maxBytes = qint64{1024} * 1024, int keepFiles = 5);

// Stops writing to the file and restores the previous handler. Safe to call when nothing is installed.
void uninstallFileLogger();

} // namespace selecta::base
