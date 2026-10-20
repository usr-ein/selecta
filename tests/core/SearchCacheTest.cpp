// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchCache.h"

#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QtEndian>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::SearchCacheStore;
using selecta::core::SearchHit;
using selecta::core::SearchOutcome;

namespace {

constexpr qint64 kHour = qint64{60} * 60 * 1000;

SearchHit hit(const QString &peer, const QString &path)
{
    SearchHit result;
    result.peer = peer;
    result.path = path.toUtf8();
    result.size = 9'876'543'210; // above 4 GiB: sizes are 64-bit
    result.extension = u"flac"_s;
    result.attributes.duration = 372;
    result.attributes.sampleRate = 44'100;
    result.attributes.bitDepth = 16;
    result.attributes.other = {{7, 1}};
    result.freeSlot = true;
    result.queueLength = 3;
    result.averageSpeed = 1'250'000;
    return result;
}

SearchOutcome outcomeWith(const QList<SearchHit> &hits)
{
    SearchOutcome outcome;
    outcome.query = u"okuma drift neon rain"_s;
    outcome.hits = hits;
    outcome.peers = 2;
    return outcome;
}

class SearchCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    selecta::storage::Database db;
    qint64 now = 1'800'000'000'000;
    SearchCacheStore store{db, [this] { return now; }};
};

} // namespace

TEST_F(SearchCacheTest, KeepsAndFindsASearch)
{
    QList<SearchHit> hits{hit(u"vinyl_ghost"_s, u"@@a\\Okuma Drift - Neon Rain.flac"_s)};
    SearchHit lossy = hit(u"bassbin"_s, u"@@b\\Okuma Drift - Neon Rain.mp3"_s);
    lossy.extension = u"mp3"_s;
    lossy.attributes = {};
    lossy.attributes.bitrate = 320;
    lossy.attributes.vbr = false;
    lossy.freeSlot = false;
    hits.append(lossy);
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith(hits)));
    now += kHour;

    const std::optional<SearchCacheStore::Entry> found = store.find(u"key"_s);
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->outcome.fromCache);
    EXPECT_EQ(found->outcome.searchedAtMs, now - kHour);
    EXPECT_EQ(found->outcome.query, u"okuma drift neon rain"_s);
    EXPECT_EQ(found->outcome.peers, 2);
    ASSERT_EQ(found->outcome.hits.size(), 2);
    EXPECT_EQ(found->outcome.hits.at(0).peer, hits.at(0).peer);
    EXPECT_EQ(found->outcome.hits.at(0).path, hits.at(0).path);
    EXPECT_EQ(found->outcome.hits.at(0).size, hits.at(0).size);
    EXPECT_EQ(found->outcome.hits.at(0).attributes, hits.at(0).attributes);
    EXPECT_EQ(found->outcome.hits.at(0).queueLength, 3U);
    EXPECT_EQ(found->outcome.hits.at(0).averageSpeed, 1'250'000U);
    EXPECT_TRUE(found->outcome.hits.at(0).freeSlot);
    EXPECT_EQ(found->outcome.hits.at(1).attributes, lossy.attributes); // a known "false" VBR survives
    EXPECT_FALSE(found->outcome.hits.at(1).freeSlot);
    EXPECT_EQ(store.count(), 1);
    EXPECT_GT(store.sizeBytes(), 0);
}

TEST_F(SearchCacheTest, ADayLaterTheSearchIsGone)
{
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
    now += SearchCacheStore::kDefaultLifetimeMs;
    EXPECT_FALSE(store.find(u"key"_s).has_value());
    EXPECT_EQ(store.count(), 0); // forgotten, not just skipped
}

TEST_F(SearchCacheTest, ASearchFromTheFutureIsntTrusted)
{
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
    now -= kHour; // the clock went back
    EXPECT_FALSE(store.find(u"key"_s).has_value());
}

TEST_F(SearchCacheTest, NothingFoundIsntKept)
{
    EXPECT_TRUE(store.store(u"key"_s, outcomeWith({})));
    SearchOutcome failed = outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)});
    failed.error = u"rate limited"_s;
    EXPECT_TRUE(store.store(u"other"_s, failed));
    EXPECT_EQ(store.count(), 0);
}

TEST_F(SearchCacheTest, ARepeatReplacesTheOlderSearch)
{
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
    now += kHour;
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s), hit(u"q"_s, u"@@b\\y.flac"_s)})));
    const auto found = store.find(u"key"_s);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->outcome.hits.size(), 2);
    EXPECT_EQ(found->outcome.searchedAtMs, now);
    EXPECT_EQ(store.count(), 1);
}

TEST_F(SearchCacheTest, TheOldestGoBeyondTheLimit)
{
    for (int i = 0; i < SearchCacheStore::kMaxSearches + 5; ++i) {
        ASSERT_TRUE(store.store(u"key %1"_s.arg(i), outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
        ++now;
    }
    EXPECT_EQ(store.count(), SearchCacheStore::kMaxSearches);
    EXPECT_FALSE(store.find(u"key 0"_s).has_value());
    EXPECT_TRUE(store.find(u"key %1"_s.arg(SearchCacheStore::kMaxSearches + 4)).has_value());
}

TEST_F(SearchCacheTest, ClearEmptiesIt)
{
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
    EXPECT_TRUE(store.clear());
    EXPECT_EQ(store.count(), 0);
    EXPECT_EQ(store.sizeBytes(), 0);
}

TEST(SearchCacheEncoding, RoundTripsEveryField)
{
    SearchHit bare;
    bare.peer = u"ü-peer"_s;
    bare.path = QByteArray("@@x\\caf\xe9.mp3"); // Latin-1 bytes from an old client stay bytes
    const QList<SearchHit> hits{hit(u"vinyl_ghost"_s, u"@@a\\Ünïcödé.flac"_s), bare};
    const auto decoded = SearchCacheStore::decode(SearchCacheStore::encode(hits));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 2);
    EXPECT_EQ(decoded->at(0).path, hits.at(0).path);
    EXPECT_EQ(decoded->at(0).attributes, hits.at(0).attributes);
    EXPECT_EQ(decoded->at(1).path, bare.path);
    EXPECT_EQ(decoded->at(1).attributes, bare.attributes);
    EXPECT_FALSE(decoded->at(1).attributes.vbr.has_value());
}

TEST(SearchCacheEncoding, DamagedBlobsAreRefused)
{
    const QByteArray good = SearchCacheStore::encode({hit(u"p"_s, u"@@a\\x.flac"_s)});
    EXPECT_FALSE(SearchCacheStore::decode({}).has_value());
    EXPECT_FALSE(SearchCacheStore::decode(good.left(good.size() / 2)).has_value());
    EXPECT_FALSE(SearchCacheStore::decode(QByteArray("not a cache entry")).has_value());

    // A header claiming a huge inflated size is refused before anything is inflated.
    QByteArray huge = good;
    qToBigEndian<quint32>(0x7FFFFFFF, huge.data());
    EXPECT_FALSE(SearchCacheStore::decode(huge).has_value());

    // Valid compression around garbage, and around a count beyond the bound.
    EXPECT_FALSE(SearchCacheStore::decode(qCompress(QByteArray(64, 'x'))).has_value());
    QByteArray raw;
    {
        QDataStream out(&raw, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_5);
        out << quint32{0x53524348} << quint16{1} << quint32{1'000'000};
    }
    EXPECT_FALSE(SearchCacheStore::decode(qCompress(raw)).has_value());
}

TEST_F(SearchCacheTest, ADamagedEntryIsForgotten)
{
    ASSERT_TRUE(store.store(u"key"_s, outcomeWith({hit(u"p"_s, u"@@a\\x.flac"_s)})));
    auto damage = db.prepare(u"UPDATE search_cache SET hits = ?1;");
    ASSERT_TRUE(damage.bind(1, QByteArray("garbage")).run());
    EXPECT_FALSE(store.find(u"key"_s).has_value());
    EXPECT_EQ(store.count(), 0);
}
