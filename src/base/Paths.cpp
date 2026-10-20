// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace selecta::base::paths {
namespace {

QString
    g_root; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once at start-up, see setRootOverride()

} // namespace

void setRootOverride(const QString &root)
{
    g_root = root.isEmpty() ? QString() : QDir::cleanPath(QDir(root).absolutePath());
}

QString rootOverride()
{
    return g_root;
}

QString appSupportDir()
{
    if (!g_root.isEmpty()) {
        return g_root + u"/data"_s;
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString cacheDir()
{
    if (!g_root.isEmpty()) {
        return g_root + u"/cache"_s;
    }
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString logDir()
{
    if (!g_root.isEmpty()) {
        return g_root + u"/logs"_s;
    }
#ifdef Q_OS_MACOS
    if (!QStandardPaths::isTestModeEnabled()) {
        return QDir::homePath() + u"/Library/Logs/"_s + QCoreApplication::applicationName();
    }
#endif
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + u"/logs"_s;
}

QString databaseFile()
{
    return appSupportDir() + u"/selecta.sqlite3"_s;
}

QString settingsFile()
{
    return appSupportDir() + u"/settings.ini"_s;
}

QString inboxDir()
{
    return appSupportDir() + u"/Inbox"_s;
}

bool ensureDir(const QString &path)
{
    return !path.isEmpty() && QDir().mkpath(path) && QFileInfo(path).isDir();
}

} // namespace selecta::base::paths
