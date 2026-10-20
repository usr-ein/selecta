// SPDX-License-Identifier: GPL-3.0-or-later
// The collection watcher (PRD LIB-2): FSEvents on macOS and the portable QFileSystemWatcher, coalesced, folders
// reported under the roots as given.
#include "collection/Watcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::collection::Watcher;

namespace {

constexpr int kWaitMs = 15000; // FSEvents' latency plus the system's own delivery, with room for a busy machine

struct MechanismCase
{
    Watcher::Mechanism mechanism = Watcher::Mechanism::Native;
    QString name;
};

class WatcherTest : public ::testing::TestWithParam<MechanismCase>
{};

void touch(const QString &path, const QByteArray &bytes = "x")
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Append));
    file.write(bytes);
}

// Every folder reported until `wanted` shows up (or the time runs out).
QStringList waitFor(QSignalSpy &spy, const QString &wanted)
{
    QStringList seen;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kWaitMs) {
        while (!spy.isEmpty()) {
            seen += spy.takeFirst().at(0).toStringList();
        }
        if (seen.contains(wanted)) {
            break;
        }
        spy.wait(500);
    }
    return seen;
}

TEST_P(WatcherTest, ReportsTheFolderOfANewFile)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString album = dir.filePath(u"Artist/Album"_s);
    ASSERT_TRUE(QDir().mkpath(album));
    Watcher watcher(GetParam().mechanism);
    QSignalSpy spy(&watcher, &Watcher::changed);
    ASSERT_TRUE(watcher.watch({dir.path()})) << watcher.errorString().toStdString();
    EXPECT_TRUE(watcher.isWatching());
    EXPECT_EQ(watcher.roots(), QStringList{QDir::cleanPath(dir.path())});
    QTest::qWait(300); // let the stream settle before the first change
    touch(album + u"/01 - Title.mp3"_s);
    const QStringList seen = waitFor(spy, album);
    EXPECT_TRUE(seen.contains(album)) << seen.join(u", ").toStdString();
    for (const QString &folder : seen) {
        EXPECT_TRUE(folder.startsWith(QDir::cleanPath(dir.path())))
            << "under the root as given: " << folder.toStdString();
    }
}

TEST_P(WatcherTest, ReportsModifiedAndDeletedFiles)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = dir.filePath(u"Singles"_s);
    ASSERT_TRUE(QDir().mkpath(folder));
    const QString song = folder + u"/Artist - Title.mp3"_s;
    touch(song);
    Watcher watcher(GetParam().mechanism);
    QSignalSpy spy(&watcher, &Watcher::changed);
    ASSERT_TRUE(watcher.watch({dir.path()}));
    QTest::qWait(300);
    ASSERT_TRUE(QFile::remove(song));
    const QStringList seen = waitFor(spy, folder);
    EXPECT_TRUE(seen.contains(folder)) << seen.join(u", ").toStdString();
}

TEST_P(WatcherTest, ABurstComesAsOneReport)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString folder = dir.filePath(u"Album"_s);
    ASSERT_TRUE(QDir().mkpath(folder));
    Watcher watcher(GetParam().mechanism);
    QSignalSpy spy(&watcher, &Watcher::changed);
    ASSERT_TRUE(watcher.watch({dir.path()}));
    QTest::qWait(300);
    for (int i = 0; i < 20; ++i) {
        touch(folder + u"/%1.mp3"_s.arg(i));
    }
    const QStringList seen = waitFor(spy, folder);
    EXPECT_TRUE(seen.contains(folder));
    EXPECT_LE(spy.count() + 1, 3) << "twenty changes in a burst, coalesced";
}

TEST_P(WatcherTest, NothingAfterStop)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Watcher watcher(GetParam().mechanism);
    const QSignalSpy spy(&watcher, &Watcher::changed);
    ASSERT_TRUE(watcher.watch({dir.path()}));
    QTest::qWait(300);
    touch(dir.filePath(u"a.mp3"_s));
    watcher.stop();
    EXPECT_FALSE(watcher.isWatching());
    EXPECT_TRUE(watcher.roots().isEmpty());
    QTest::qWait(Watcher::kLatencyMs + 1500);
    EXPECT_EQ(spy.count(), 0);
}

TEST_P(WatcherTest, MissingRootsCantBeWatched)
{
    Watcher watcher(GetParam().mechanism);
    EXPECT_FALSE(watcher.watch({u"/nonexistent/selecta-test/root"_s}));
    EXPECT_FALSE(watcher.isWatching());
    EXPECT_FALSE(watcher.errorString().isEmpty());
    EXPECT_FALSE(watcher.watch({}));
}

TEST_P(WatcherTest, MissingRootsAreLeftOut)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Watcher watcher(GetParam().mechanism);
    ASSERT_TRUE(watcher.watch({u"/nonexistent/selecta-test/root"_s, dir.path()}));
    EXPECT_EQ(watcher.roots(), QStringList{QDir::cleanPath(dir.path())});
}

TEST_P(WatcherTest, DestroyingWhileEventsArePendingIsSafe)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (int round = 0; round < 3; ++round) {
        auto watcher = std::make_unique<Watcher>(GetParam().mechanism);
        ASSERT_TRUE(watcher->watch({dir.path()}));
        touch(dir.filePath(u"x%1.mp3"_s.arg(round)));
        QTest::qWait(50);
        watcher.reset(); // with a change in flight
    }
    QTest::qWait(Watcher::kLatencyMs + 500); // queued deliveries to a destroyed watcher are dropped
    SUCCEED();
}

INSTANTIATE_TEST_SUITE_P(Mechanisms, WatcherTest,
                         ::testing::Values(MechanismCase{Watcher::Mechanism::Native, u"Native"_s},
                                           MechanismCase{Watcher::Mechanism::Portable, u"Portable"_s}),
                         [](const ::testing::TestParamInfo<MechanismCase> &info) {
                             return info.param.name.toStdString();
                         });

} // namespace
