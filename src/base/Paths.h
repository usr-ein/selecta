// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

// Standard locations (PLAN §2.5). The application name must be set before calling these.
namespace selecta::base::paths {

QString appSupportDir();   // ~/Library/Application Support/Selecta
QString cacheDir();        // ~/Library/Caches/Selecta
QString logDir();          // ~/Library/Logs/Selecta
QString databaseFile();    // <appSupportDir>/selecta.sqlite3
QString settingsFile();    // <appSupportDir>/settings.ini
QString inboxDir();        // <appSupportDir>/Inbox: Selecta's own, where downloads wait to be filed (never the user's)

// Puts every location above under `root` (data, cache and logs sub-folders), for isolated test and debugging runs
// (`Selecta --data-dir`). An empty string restores the standard locations. Not thread-safe: call it at start-up,
// before any other thread runs.
void setRootOverride(const QString &root);
QString rootOverride();

// Creates `path` and its parents. Returns false if it doesn't exist as a folder afterwards.
bool ensureDir(const QString &path);

} // namespace selecta::base::paths
