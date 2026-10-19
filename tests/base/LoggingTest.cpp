// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Logging.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(u'\n', Qt::SkipEmptyParts);
}

qsizetype countContaining(const QStringList &lines, const QString &needle)
{
    return std::count_if(lines.begin(), lines.end(), [&](const QString &line) { return line.contains(needle); });
}

// Removes the logger even when an assertion fails half way.
struct LoggerGuard
{
    LoggerGuard() = default;
    LoggerGuard(const LoggerGuard &) = delete;
    LoggerGuard &operator=(const LoggerGuard &) = delete;
    LoggerGuard(LoggerGuard &&) = delete;
    LoggerGuard &operator=(LoggerGuard &&) = delete;
    ~LoggerGuard() { selecta::base::uninstallFileLogger(); }
};

} // namespace

TEST(Logging, WritesMessagesWithLevelAndCategory)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const LoggerGuard guard;
    ASSERT_TRUE(selecta::base::installFileLogger(dir.path()));
    qCInfo(lcBase) << "hello file";
    selecta::base::uninstallFileLogger();

    const QStringList lines = readLines(dir.filePath(u"selecta.log"_s));
    ASSERT_EQ(countContaining(lines, u"hello file"_s), 1);
    const QStringList matching = lines.filter(u"hello file"_s);
    const QString &line = matching.constFirst();
    EXPECT_TRUE(line.contains(u" info selecta.base: "_s)) << line.toStdString();
    EXPECT_TRUE(line.endsWith(u"hello file"_s));
}

TEST(Logging, InstallingTwiceDoesNotDuplicateOrRecurse)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const LoggerGuard guard;
    ASSERT_TRUE(selecta::base::installFileLogger(dir.path()));
    ASSERT_TRUE(selecta::base::installFileLogger(dir.path())); // must not chain to itself
    qCInfo(lcBase) << "once";
    selecta::base::uninstallFileLogger();

    EXPECT_EQ(countContaining(readLines(dir.filePath(u"selecta.log"_s)), u"once"_s), 1);
}

TEST(Logging, UninstallStopsWriting)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        const LoggerGuard guard;
        ASSERT_TRUE(selecta::base::installFileLogger(dir.path()));
    }
    selecta::base::uninstallFileLogger(); // a second uninstall is harmless
    qCInfo(lcBase) << "after uninstall";
    EXPECT_EQ(countContaining(readLines(dir.filePath(u"selecta.log"_s)), u"after uninstall"_s), 0);
}

TEST(Logging, RotatesAndKeepsAtMostKeepFiles)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const LoggerGuard guard;
    ASSERT_TRUE(selecta::base::installFileLogger(dir.path(), 512, 3));
    for (int i = 0; i < 200; ++i) {
        qCInfo(lcBase) << "line" << i << QString(40, u'x');
    }
    selecta::base::uninstallFileLogger();

    const QStringList files = QDir(dir.path()).entryList({u"selecta*.log"_s}, QDir::Files, QDir::Name);
    EXPECT_EQ(files, (QStringList{u"selecta.1.log"_s, u"selecta.2.log"_s, u"selecta.log"_s}));
    // The newest message is in the current file.
    EXPECT_EQ(countContaining(readLines(dir.filePath(u"selecta.log"_s)), u"line 199 "_s), 1);
}

TEST(Logging, ConcurrentWritersLoseNoLines)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const LoggerGuard guard;
    ASSERT_TRUE(selecta::base::installFileLogger(dir.path(), qint64{64} * 1024 * 1024, 1));

    constexpr int kThreads = 8;
    constexpr int kMessages = 250;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kMessages; ++i) {
                qCInfo(lcBase).noquote() << u"concurrent t%1 m%2"_s.arg(t).arg(i);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    selecta::base::uninstallFileLogger();

    EXPECT_EQ(countContaining(readLines(dir.filePath(u"selecta.log"_s)), u"concurrent t"_s), kThreads * kMessages);
}

TEST(Logging, ReturnsFalseWhenTheDirectoryCannotBeCreated)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile blocker(dir.filePath(u"not-a-dir"_s));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const LoggerGuard guard;
    EXPECT_FALSE(selecta::base::installFileLogger(dir.filePath(u"not-a-dir/logs"_s)));
}
