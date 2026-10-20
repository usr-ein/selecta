// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/ByteSource.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace selecta::audio;

namespace {

QByteArray pattern(int size)
{
    QByteArray data(size, '\0');
    for (int i = 0; i < size; ++i) {
        data[i] = static_cast<char>(i * 7);
    }
    return data;
}

} // namespace

TEST(FileSource, ReadsWithinBounds)
{
    const QTemporaryDir dir;
    const QString path = dir.filePath(u"a.bin"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(pattern(1000));
    file.close();

    const Result<std::shared_ptr<FileSource>> opened = FileSource::open(path);
    ASSERT_TRUE(opened.ok()) << opened.error().toString().toStdString();
    const std::shared_ptr<FileSource> &source = opened.value();
    EXPECT_EQ(source->totalSize(), 1000);
    EXPECT_TRUE(source->snapshot().isComplete());
    std::vector<char> buffer(2000);
    EXPECT_EQ(source->read(990, buffer.data(), 100), 10);
    EXPECT_EQ(buffer[0], static_cast<char>(990 * 7));
    EXPECT_EQ(source->read(1000, buffer.data(), 10), 0);
    EXPECT_EQ(source->read(5000, buffer.data(), 10), 0);
    EXPECT_EQ(source->read(0, buffer.data(), 0), 0);
    EXPECT_EQ(source->read(-1, buffer.data(), 10), -1);
    EXPECT_EQ(source->lastError().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(source->read(0, buffer.data(), -5), -1);
}

TEST(FileSource, RefusesMissingFilesAndDirectories)
{
    const QTemporaryDir dir;
    const Result<std::shared_ptr<FileSource>> missing = FileSource::open(dir.filePath(u"nope"_s));
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::OpenFailed);
    const Result<std::shared_ptr<FileSource>> directory = FileSource::open(dir.path());
    ASSERT_FALSE(directory.ok());
    EXPECT_EQ(directory.error().code, ErrorCode::OpenFailed);
}

TEST(FileSource, EmptyFile)
{
    const QTemporaryDir dir;
    const QString path = dir.filePath(u"empty"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    const Result<std::shared_ptr<FileSource>> opened = FileSource::open(path);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value()->totalSize(), 0);
    char byte = 0;
    EXPECT_EQ(opened.value()->read(0, &byte, 1), 0);
}

TEST(MemorySource, GrowsMonotonically)
{
    MemorySource source(pattern(100), 10);
    EXPECT_EQ(source.state(), SourceState::Growing);
    EXPECT_EQ(source.available(), 10);
    std::vector<char> buffer(100);
    EXPECT_EQ(source.read(0, buffer.data(), 100), 10); // never past available
    EXPECT_EQ(source.read(10, buffer.data(), 1), 0);
    source.setAvailable(50);
    source.setAvailable(20); // ignored: never decreases
    EXPECT_EQ(source.available(), 50);
    source.setAvailable(500); // clamped
    EXPECT_EQ(source.available(), 100);
    source.setComplete();
    EXPECT_TRUE(source.snapshot().isComplete());
    source.setFailed(); // ignored after completion
    EXPECT_EQ(source.state(), SourceState::Complete);
}

TEST(MemorySource, FailureKeepsPublishedBytes)
{
    MemorySource source(pattern(100), 40);
    source.setFailed();
    EXPECT_EQ(source.state(), SourceState::Failed);
    EXPECT_EQ(source.available(), 40);
    source.setAvailable(80); // ignored after failure
    EXPECT_EQ(source.available(), 40);
    std::vector<char> buffer(100);
    EXPECT_EQ(source.read(0, buffer.data(), 100), 40);
}

TEST(GrowingFile, ReadsOnlyPublishedBytes)
{
    const QTemporaryDir dir;
    const QString path = dir.filePath(u"preview.part"_s);
    QFile writer(path);
    ASSERT_TRUE(writer.open(QIODevice::WriteOnly));
    const QByteArray data = pattern(10000);
    writer.write(data.left(3000));
    writer.flush();

    const Result<std::shared_ptr<GrowingFile>> opened = GrowingFile::open(path, data.size());
    ASSERT_TRUE(opened.ok());
    const std::shared_ptr<GrowingFile> &source = opened.value();
    EXPECT_EQ(source->available(), 0); // nothing published yet, even though bytes are on disk
    std::vector<char> buffer(10000);
    EXPECT_EQ(source->read(0, buffer.data(), 100), 0);
    source->setAvailable(3000);
    EXPECT_EQ(source->read(0, buffer.data(), 10000), 3000);
    EXPECT_EQ(std::memcmp(buffer.data(), data.constData(), 3000), 0);

    writer.write(data.mid(3000));
    writer.flush();
    source->setComplete();
    EXPECT_EQ(source->available(), 10000);
    EXPECT_EQ(source->read(9990, buffer.data(), 100), 10);
}

TEST(GrowingFile, ReportsTruncationBehindItsBack)
{
    const QTemporaryDir dir;
    const QString path = dir.filePath(u"preview.part"_s);
    QFile writer(path);
    ASSERT_TRUE(writer.open(QIODevice::WriteOnly));
    writer.write(pattern(100));
    writer.flush();
    const Result<std::shared_ptr<GrowingFile>> opened = GrowingFile::open(path, 1000);
    ASSERT_TRUE(opened.ok());
    opened.value()->setAvailable(500); // claims bytes that were never written
    std::vector<char> buffer(500);
    EXPECT_EQ(opened.value()->read(0, buffer.data(), 500), -1);
    EXPECT_EQ(opened.value()->lastError().code, ErrorCode::ReadFailed);
}

TEST(GrowingFile, RejectsBadArguments)
{
    const QTemporaryDir dir;
    EXPECT_EQ(GrowingFile::open(dir.filePath(u"missing"_s), 10).error().code, ErrorCode::OpenFailed);
    const QString path = dir.filePath(u"x"_s);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    EXPECT_EQ(GrowingFile::open(path, 0).error().code, ErrorCode::InvalidArgument);
}

TEST(Subscription, ListenersFollowChangesUntilReset)
{
    MemorySource source(pattern(100), 0);
    int calls = 0;
    Subscription subscription = source.subscribe([&] { ++calls; });
    source.setAvailable(10);
    source.setAvailable(10); // no change, no call
    source.setAvailable(20);
    EXPECT_EQ(calls, 2);
    subscription.reset();
    source.setAvailable(30);
    EXPECT_EQ(calls, 2);
    EXPECT_FALSE(subscription.isActive());
}

TEST(Subscription, ListenerMayDropItself)
{
    MemorySource source(pattern(100), 0);
    int calls = 0;
    Subscription subscription;
    subscription = source.subscribe([&] {
        ++calls;
        subscription.reset();
    });
    source.setAvailable(10);
    source.setAvailable(20);
    EXPECT_EQ(calls, 1);
}

TEST(Subscription, MovedSubscriptionKeepsListening)
{
    MemorySource source(pattern(100), 0);
    int calls = 0;
    Subscription a = source.subscribe([&] { ++calls; });
    const Subscription b = std::move(a);
    source.setAvailable(5);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(b.isActive());
}

// A writer publishing bytes while readers read and subscribe/unsubscribe: under TSan this checks the memory ordering
// of available/state and the listener hand-off.
TEST(GrowingSources, ConcurrentWriterAndReaders)
{
    const QByteArray data = pattern(65536);
    auto source = std::make_shared<MemorySource>(data, 0);
    std::atomic<bool> done{false};
    std::atomic<int> notifications{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&, r] {
            std::vector<char> buffer(4096);
            while (!done.load()) {
                const Subscription s = source->subscribe([&] { notifications.fetch_add(1); });
                const SourceSnapshot snapshot = source->snapshot();
                const qint64 position = ((snapshot.available / 2) + (qint64{r} * 97)) % (snapshot.available + 1);
                const qint64 got = source->read(position, buffer.data(), 4096);
                if (got < 0 || position + got > source->available() ||
                    (got > 0 && buffer[0] != data[static_cast<int>(position)])) {
                    bad.fetch_add(1);
                }
                if (snapshot.isComplete() && snapshot.available != data.size()) {
                    bad.fetch_add(1);
                }
            }
        });
    }
    for (qint64 n = 0; n <= data.size(); n += 512) {
        source->setAvailable(n);
    }
    source->setComplete();
    done = true;
    for (std::thread &t : readers) {
        t.join();
    }
    EXPECT_EQ(bad.load(), 0);
}
