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
    const QStringList all{paths::appSupportDir(), paths::cacheDir(), paths::logDir(), paths::defaultInboxDir()};
    for (const QString &path : all) {
        EXPECT_FALSE(path.isEmpty());
        EXPECT_TRUE(QDir::isAbsolutePath(path)) << path.toStdString();
    }
    EXPECT_EQ(QSet<QString>(all.begin(), all.end()).size(), all.size());
    EXPECT_TRUE(paths::databaseFile().startsWith(paths::appSupportDir() + u'/'));
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
