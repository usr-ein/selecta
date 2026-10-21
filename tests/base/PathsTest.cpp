// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Paths.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
namespace paths = selecta::base::paths;

TEST(Paths, AreAbsoluteAndDistinct)
{
    const QStringList all{paths::appSupportDir(), paths::cacheDir(), paths::logDir(), paths::inboxDir()};
    for (const QString &path : all) {
        EXPECT_FALSE(path.isEmpty());
        EXPECT_TRUE(QDir::isAbsolutePath(path)) << path.toStdString();
    }
    EXPECT_EQ(QSet<QString>(all.begin(), all.end()).size(), all.size());
    EXPECT_TRUE(paths::databaseFile().startsWith(paths::appSupportDir() + u'/'));
}

TEST(Paths, TheInboxIsInTheDataFolder)
{
    // Selecta's own, never chosen by the user (P-001): next to its library, under --data-dir too.
    EXPECT_EQ(paths::inboxDir(), paths::appSupportDir() + u"/Inbox"_s);
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    paths::setRootOverride(dir.path());
    EXPECT_EQ(paths::inboxDir(), QDir::cleanPath(dir.path()) + u"/data/Inbox"_s);
    paths::setRootOverride({});
    EXPECT_EQ(paths::inboxDir(), paths::appSupportDir() + u"/Inbox"_s);
}

TEST(Paths, EnsureDirCreatesNestedFolders)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString nested = dir.filePath(u"a/b/c"_s);
    EXPECT_TRUE(paths::ensureDir(nested));
    EXPECT_TRUE(QFileInfo(nested).isDir());
    EXPECT_TRUE(paths::ensureDir(nested)); // already there
    EXPECT_FALSE(paths::ensureDir(QString()));
}
