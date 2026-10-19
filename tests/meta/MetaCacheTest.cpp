// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/MetaCache.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::base::ManualClock;
using selecta::meta::MemoryMetaCache;

TEST(MemoryMetaCache, StoresAndReturnsWithinTheLifetime)
{
    ManualClock clock;
    MemoryMetaCache cache(clock);
    cache.put(u"spotify/track/1"_s, "body", 60s);
    ASSERT_TRUE(cache.get(u"spotify/track/1"_s).has_value());
    EXPECT_EQ(*cache.get(u"spotify/track/1"_s), "body");
    clock.advance(59s);
    EXPECT_TRUE(cache.get(u"spotify/track/1"_s).has_value());
}

TEST(MemoryMetaCache, ExpiresAtTheEndOfTheLifetime)
{
    ManualClock clock;
    MemoryMetaCache cache(clock);
    cache.put(u"k"_s, "v", 60s);
    clock.advance(60s);
    EXPECT_FALSE(cache.get(u"k"_s).has_value());
    EXPECT_EQ(cache.count(), 0);
    EXPECT_EQ(cache.sizeBytes(), 0);
}

TEST(MemoryMetaCache, NonPositiveLifetimeStoresNothingAndDropsAnOldEntry)
{
    ManualClock clock;
    MemoryMetaCache cache(clock);
    cache.put(u"k"_s, "v", 60s);
    cache.put(u"k"_s, "w", 0s);
    EXPECT_FALSE(cache.get(u"k"_s).has_value());
    cache.put(u"n"_s, "v", -5s);
    EXPECT_FALSE(cache.get(u"n"_s).has_value());
}

TEST(MemoryMetaCache, ReplacingAnEntryKeepsTheSizeExact)
{
    ManualClock clock;
    MemoryMetaCache cache(clock);
    cache.put(u"k"_s, QByteArray(100, 'a'), 60s);
    const qint64 first = cache.sizeBytes();
    cache.put(u"k"_s, QByteArray(10, 'b'), 60s);
    EXPECT_EQ(cache.count(), 1);
    EXPECT_EQ(cache.sizeBytes(), first - 90);
    EXPECT_EQ(*cache.get(u"k"_s), QByteArray(10, 'b'));
}

TEST(MemoryMetaCache, EvictsLeastRecentlyUsedBeyondTheByteBound)
{
    ManualClock clock;
    MemoryMetaCache cache(clock, 1000);
    cache.put(u"a"_s, QByteArray(300, 'a'), 60s);
    cache.put(u"b"_s, QByteArray(300, 'b'), 60s);
    ASSERT_TRUE(cache.get(u"a"_s).has_value());   // a is now the most recently used
    cache.put(u"c"_s, QByteArray(300, 'c'), 60s); // must evict b
    EXPECT_TRUE(cache.get(u"a"_s).has_value());
    EXPECT_FALSE(cache.get(u"b"_s).has_value());
    EXPECT_TRUE(cache.get(u"c"_s).has_value());
    EXPECT_LE(cache.sizeBytes(), 1000);
}

TEST(MemoryMetaCache, AnEntryLargerThanTheBoundIsNotStored)
{
    ManualClock clock;
    MemoryMetaCache cache(clock, 100);
    cache.put(u"small"_s, "x", 60s);
    cache.put(u"huge"_s, QByteArray(1000, 'h'), 60s);
    EXPECT_FALSE(cache.get(u"huge"_s).has_value());
    EXPECT_TRUE(cache.get(u"small"_s).has_value()); // nothing was evicted for it
}

TEST(MemoryMetaCache, RemovesByPrefix)
{
    ManualClock clock;
    MemoryMetaCache cache(clock);
    cache.put(u"spotify/search/a"_s, "1", 60s);
    cache.put(u"spotify/track/b"_s, "2", 60s);
    cache.put(u"deezer/track/c"_s, "3", 60s);
    cache.removePrefix(u"spotify/"_s);
    EXPECT_FALSE(cache.get(u"spotify/search/a"_s).has_value());
    EXPECT_FALSE(cache.get(u"spotify/track/b"_s).has_value());
    EXPECT_TRUE(cache.get(u"deezer/track/c"_s).has_value());
    EXPECT_EQ(cache.count(), 1);
}

TEST(MemoryMetaCache, ManyEntriesStayBounded)
{
    ManualClock clock;
    MemoryMetaCache cache(clock, 10'000);
    for (int i = 0; i < 5000; ++i) {
        cache.put(u"key/%1"_s.arg(i), QByteArray(50, 'x'), 60s);
    }
    EXPECT_LE(cache.sizeBytes(), 10'000);
    EXPECT_TRUE(cache.get(u"key/4999"_s).has_value());
    EXPECT_FALSE(cache.get(u"key/0"_s).has_value());
}
