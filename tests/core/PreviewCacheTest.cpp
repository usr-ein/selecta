// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PreviewCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::core::PreviewCache;

namespace {

constexpr qint64 kDay = qint64{24} * 3600 * 1000;
constexpr qint64 kStart = 1'800'000'000'000;

class PreviewCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        open();
    }

    // Opens the folder as a launch does (closing the cache open before).
    void open(qint64 maxBytes = PreviewCache::kDefaultMaxBytes)
    {
        cache.reset();
        PreviewCache::Options options;
        options.maxBytes = maxBytes;
        options.now = [this] { return now; };
        cache = std::make_unique<PreviewCache>(folder(), options);
        ASSERT_TRUE(cache->isUsable());
    }

    QString folder() const { return dir.filePath(u"previews"_s); }

    static bool write(const QString &path, qint64 bytes)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(QByteArray(bytes, 'x')) == bytes;
    }

    // A preview written to the end and kept under `key`; its path.
    QString keep(const QString &key, qint64 bytes, const QString &note = {})
    {
        const QString path = cache->reserve(key.section(u'\n', -1) + u".flac"_s);
        EXPECT_FALSE(path.isEmpty());
        EXPECT_TRUE(write(path, bytes));
        EXPECT_TRUE(cache->commit(key, path, note));
        cache->unpin(path); // done playing
        return path;
    }

    QTemporaryDir dir;
    qint64 now = kStart;
    std::unique_ptr<PreviewCache> cache;
};

} // namespace

TEST_F(PreviewCacheTest, KeepsCompleteFilesAcrossLaunches)
{
    const QString path = keep(u"peer\nghost\nYQ=="_s, 1000, u"a note"_s);
    open();
    const std::optional<PreviewCache::File> file = cache->find(u"peer\nghost\nYQ=="_s);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(QFileInfo(file->path).canonicalFilePath(), QFileInfo(path).canonicalFilePath());
    EXPECT_EQ(file->bytes, 1000);
    EXPECT_EQ(file->note, u"a note"_s);
    EXPECT_FALSE(cache->find(u"peer\nghost\nYg=="_s).has_value());
}

TEST_F(PreviewCacheTest, AQuitOrCrashLeavesNothingHalfWritten)
{
    const QString kept = keep(u"kept"_s, 1000);
    const QString partial = cache->reserve(u"partial.flac"_s); // being written when Selecta stopped
    ASSERT_TRUE(write(partial, 500));
    ASSERT_TRUE(QDir().mkpath(folder() + u"/YouTube"_s));
    const QString youtube = folder() + u"/YouTube/AAAAAAAAAAA.m4a"_s; // yt-dlp was writing it
    ASSERT_TRUE(write(youtube, 700));
    const QString older = folder() + u"/1-from-an-earlier-version.flac"_s;
    ASSERT_TRUE(write(older, 300));

    open();
    EXPECT_TRUE(QFile::exists(kept));
    EXPECT_FALSE(QFile::exists(partial));
    EXPECT_FALSE(QFile::exists(youtube));
    EXPECT_FALSE(QFile::exists(older));
    EXPECT_EQ(cache->totalBytes(), 1000);
}

TEST_F(PreviewCacheTest, AFileThatChangedOnDiskIsDropped)
{
    const QString path = keep(u"grew"_s, 1000);
    ASSERT_TRUE(write(path, 10)); // appended to behind the cache's back
    EXPECT_FALSE(cache->find(u"grew"_s).has_value());
    EXPECT_FALSE(QFile::exists(path));

    const QString shrunk = keep(u"shrunk"_s, 1000);
    {
        QFile file(shrunk);
        ASSERT_TRUE(file.resize(10));
    }
    open(); // checked at launch too
    EXPECT_FALSE(cache->find(u"shrunk"_s).has_value());
    EXPECT_FALSE(QFile::exists(shrunk));
}

TEST_F(PreviewCacheTest, AFileGoneFromTheDiskIsForgotten)
{
    const QString path = keep(u"purged"_s, 1000);
    ASSERT_TRUE(QFile::remove(path)); // macOS empties caches when the disk fills up
    EXPECT_FALSE(cache->find(u"purged"_s).has_value());
    open();
    EXPECT_FALSE(cache->find(u"purged"_s).has_value());
}

TEST_F(PreviewCacheTest, TheLeastRecentlyUsedGoFirstBeyondTheBound)
{
    open(3000);
    const QString a = keep(u"a"_s, 1000);
    now += 1000;
    const QString b = keep(u"b"_s, 1000);
    now += 1000;
    const QString c = keep(u"c"_s, 1000);
    now += 1000;
    ASSERT_TRUE(cache->find(u"a"_s).has_value()); // heard again: now the most recent
    now += 1000;
    const QString d = keep(u"d"_s, 1000);

    EXPECT_TRUE(QFile::exists(a));
    EXPECT_FALSE(QFile::exists(b)); // the least recently used
    EXPECT_TRUE(QFile::exists(c));
    EXPECT_TRUE(QFile::exists(d));
    EXPECT_FALSE(cache->find(u"b"_s).has_value());
    EXPECT_EQ(cache->totalBytes(), 3000);

    open(2000); // a smaller bound at the next launch: the oldest go then
    EXPECT_FALSE(cache->find(u"c"_s).has_value());
    EXPECT_TRUE(cache->find(u"a"_s).has_value());
    EXPECT_TRUE(cache->find(u"d"_s).has_value());
}

TEST_F(PreviewCacheTest, FilesInUseAreNeverEvicted)
{
    open(2500);
    const QString playing = cache->reserve(u"playing.flac"_s);
    ASSERT_TRUE(write(playing, 1000));
    ASSERT_TRUE(cache->commit(u"playing"_s, playing)); // still pinned: playing
    now += 1000;
    const QString b = keep(u"b"_s, 1000);
    now += 1000;
    const QString c = keep(u"c"_s, 1000);
    // Over the bound: the least recently used is in use, so the next one goes instead.
    EXPECT_TRUE(QFile::exists(playing));
    EXPECT_FALSE(QFile::exists(b));
    EXPECT_TRUE(QFile::exists(c));
    cache->unpin(playing);
}

TEST_F(PreviewCacheTest, FilesUnusedForAMonthExpire)
{
    const QString old = keep(u"old"_s, 1000);
    const QString heard = keep(u"heard"_s, 1000);
    cache->remember(u"pick"_s, u"AAAAAAAAAAA"_s);
    now += 20 * kDay;
    ASSERT_TRUE(cache->find(u"heard"_s).has_value()); // heard again on day 20
    now += 11 * kDay;

    open(); // day 31
    EXPECT_FALSE(QFile::exists(old));
    EXPECT_FALSE(cache->find(u"old"_s).has_value());
    EXPECT_TRUE(cache->find(u"heard"_s).has_value());
    EXPECT_FALSE(cache->recall(u"pick"_s).has_value());

    now += 31 * kDay; // found expired while running, too
    EXPECT_FALSE(cache->find(u"heard"_s).has_value());
    EXPECT_FALSE(QFile::exists(heard));
}

TEST_F(PreviewCacheTest, RemoveDeletesAFileEvenWhilePlaying)
{
    const QString path = cache->reserve(u"discarded.flac"_s);
    ASSERT_TRUE(write(path, 1000));
    ASSERT_TRUE(cache->commit(u"discarded"_s, path)); // pinned: playing
    EXPECT_TRUE(cache->remove(u"discarded"_s));
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_FALSE(cache->find(u"discarded"_s).has_value());
    EXPECT_FALSE(cache->remove(u"discarded"_s));
    cache->unpin(path);
    open();
    EXPECT_FALSE(cache->find(u"discarded"_s).has_value());
}

TEST_F(PreviewCacheTest, AnAbandonedFileGoesOnceNobodyHoldsIt)
{
    const QString path = cache->reserve(u"skipped.flac"_s);
    ASSERT_TRUE(write(path, 1000));
    cache->pin(path); // a second holder
    cache->abandon(path);
    EXPECT_TRUE(QFile::exists(path));
    cache->abandon(path);
    EXPECT_FALSE(QFile::exists(path));

    const QString kept = keep(u"kept"_s, 1000);
    cache->pin(kept);
    cache->abandon(kept); // a file kept under a key stays
    EXPECT_TRUE(QFile::exists(kept));
}

TEST_F(PreviewCacheTest, CommittingAKeyAgainReplacesItsFile)
{
    const QString first = keep(u"key"_s, 1000);
    const QString second = keep(u"key"_s, 2000);
    EXPECT_FALSE(QFile::exists(first));
    EXPECT_EQ(cache->find(u"key"_s)->bytes, 2000);
    // A file belongs to one key.
    ASSERT_TRUE(cache->commit(u"other"_s, second));
    EXPECT_FALSE(cache->find(u"key"_s).has_value());
    EXPECT_TRUE(QFile::exists(second));
}

TEST_F(PreviewCacheTest, OnlyFilesInsideTheFolderAreKept)
{
    const QString outside = dir.filePath(u"outside.flac"_s);
    ASSERT_TRUE(write(outside, 1000));
    EXPECT_FALSE(cache->commit(u"outside"_s, outside));
    EXPECT_FALSE(cache->commit(u"traversal"_s, folder() + u"/../outside.flac"_s));
    const QString empty = cache->reserve(u"empty.flac"_s);
    EXPECT_FALSE(cache->commit(u"empty"_s, empty)); // nothing arrived
    const QString link = folder() + u"/link.flac"_s;
    ASSERT_TRUE(QFile::link(outside, link));
    EXPECT_FALSE(cache->commit(u"link"_s, link));
    EXPECT_FALSE(cache->find(u"outside"_s).has_value());
    open();
    EXPECT_TRUE(QFile::exists(outside)); // a link inside the folder goes, never what it points to
    EXPECT_FALSE(QFileInfo(link).isSymLink());
}

TEST_F(PreviewCacheTest, AnIndexNamingFilesOutsideTheFolderTouchesNothingThere)
{
    const QString outside = dir.filePath(u"precious.flac"_s);
    ASSERT_TRUE(write(outside, 1000));
    cache.reset();
    QFile index(folder() + u"/index.json"_s);
    ASSERT_TRUE(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
    index.write(R"({"version":1,"files":[
        {"key":"a","file":"../precious.flac","bytes":1000,"used":1800000000000},
        {"key":"b","file":"/etc/hosts","bytes":1000,"used":1800000000000},
        {"key":"c","file":"YouTube/../../precious.flac","bytes":1000,"used":1800000000000}],"notes":[]})");
    index.close();
    open(1); // a tiny bound: anything it believed in would be evicted
    for (const QString &key : {u"a"_s, u"b"_s, u"c"_s}) {
        EXPECT_FALSE(cache->find(key).has_value());
    }
    cache->clear();
    EXPECT_TRUE(QFile::exists(outside));
    EXPECT_TRUE(QFile::exists(u"/etc/hosts"_s));
}

TEST_F(PreviewCacheTest, ADamagedIndexStartsEmpty)
{
    const QString kept = keep(u"kept"_s, 1000);
    cache.reset();
    for (const QByteArray &damage :
         {QByteArray("{not json"), QByteArray(R"({"version":99,"files":[]})"), QByteArray(R"([1,2,3])")}) {
        QFile index(folder() + u"/index.json"_s);
        ASSERT_TRUE(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
        index.write(damage);
        index.close();
        open();
        EXPECT_FALSE(cache->find(u"kept"_s).has_value());
        EXPECT_FALSE(QFile::exists(kept)); // nothing vouches for it
        EXPECT_EQ(cache->totalBytes(), 0);
        cache.reset();
    }
    open();
    EXPECT_FALSE(keep(u"again"_s, 10).isEmpty()); // usable again
}

TEST_F(PreviewCacheTest, NotesAreRememberedAcrossLaunches)
{
    cache->remember(u"youtube-pick\nkestrel moon glass harbour\n391"_s, u"KmTopic00Ex"_s);
    open();
    EXPECT_EQ(cache->recall(u"youtube-pick\nkestrel moon glass harbour\n391"_s), u"KmTopic00Ex"_s);
    cache->forget(u"youtube-pick\nkestrel moon glass harbour\n391"_s);
    EXPECT_FALSE(cache->recall(u"youtube-pick\nkestrel moon glass harbour\n391"_s).has_value());
    cache->remember(QString(PreviewCache::kMaxTextChars + 1, u'k'), u"too long"_s);
    EXPECT_FALSE(cache->recall(QString(PreviewCache::kMaxTextChars + 1, u'k')).has_value());
}

TEST_F(PreviewCacheTest, SaysItsSizeAndEmptiesButForWhatPlays)
{
    keep(u"a"_s, 1000);
    keep(u"b"_s, 2000);
    const QString playing = cache->reserve(u"playing.flac"_s);
    ASSERT_TRUE(write(playing, 500));
    ASSERT_TRUE(QDir().mkpath(folder() + u"/YouTube"_s));
    ASSERT_TRUE(write(folder() + u"/YouTube/BBBBBBBBBBB.m4a"_s, 700)); // a stray
    cache->remember(u"pick"_s, u"BBBBBBBBBBB"_s);
    EXPECT_EQ(cache->totalBytes(), 4200); // what the folder holds, the index aside

    cache->clear();
    EXPECT_EQ(cache->totalBytes(), 500); // the preview playing stays
    EXPECT_TRUE(QFile::exists(playing));
    EXPECT_FALSE(cache->recall(u"pick"_s).has_value());
    cache->abandon(playing);
    EXPECT_EQ(cache->totalBytes(), 0);
}

TEST_F(PreviewCacheTest, AFolderThatCantBeMadeIsUnusable)
{
    const QString file = dir.filePath(u"a-file"_s);
    ASSERT_TRUE(write(file, 1));
    PreviewCache blocked(file + u"/previews"_s);
    EXPECT_FALSE(blocked.isUsable());
    EXPECT_TRUE(blocked.reserve(u"x.flac"_s).isEmpty());
    EXPECT_FALSE(blocked.find(u"x"_s).has_value());
    EXPECT_EQ(blocked.totalBytes(), 0);
    const PreviewCache none{QString()};
    EXPECT_FALSE(none.isUsable());
}
