// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/InboxStore.h"

#include "storage/Database.h"
#include "storage/Migrations.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::library::InboxItem;
using selecta::library::InboxStore;

namespace {

class InboxStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    static InboxItem single(const QString &path, qint64 createdAt = 1000)
    {
        InboxItem item;
        item.relativePath = path;
        item.artist = u"Okuma Drift"_s;
        item.title = u"Neon Rain"_s;
        item.format = u"MP3"_s;
        item.bitrate = 320;
        item.sourceFormat = u"FLAC 16/44.1"_s;
        item.tagSource = u"spotify"_s;
        item.peer = u"vinyl_ghost"_s;
        item.remotePath = QByteArray("@@vinyl\\Music\\Okuma Drift - Neon Rain.flac");
        item.durationMs = 372'000;
        item.sizeBytes = 14'900'000;
        item.createdAt = createdAt;
        return item;
    }

    selecta::storage::Database db;
    InboxStore store{db};
};

} // namespace

TEST_F(InboxStoreTest, RoundTripsEveryField)
{
    InboxItem item = single(u"Singles/Okuma Drift - Neon Rain.mp3"_s);
    item.year = 2024;
    item.releaseJson = uR"({"isrc":"GB0000000001"})"_s;
    const auto id = store.add(item);
    ASSERT_TRUE(id.has_value()) << db.lastError().toStdString();

    const auto loaded = store.find(*id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->relativePath, item.relativePath);
    EXPECT_EQ(loaded->state, InboxItem::State::Processing);
    EXPECT_EQ(loaded->bitrate, 320);
    EXPECT_EQ(loaded->year, 2024);
    EXPECT_FALSE(loaded->sampleRate.has_value());
    EXPECT_FALSE(loaded->trackCount.has_value());
    EXPECT_EQ(loaded->remotePath, item.remotePath);
    EXPECT_EQ(loaded->releaseJson, item.releaseJson);
    EXPECT_TRUE(loaded->error.isEmpty());
}

TEST_F(InboxStoreTest, PathsAreUnique)
{
    ASSERT_TRUE(store.add(single(u"Singles/a.mp3"_s)).has_value());
    EXPECT_FALSE(store.add(single(u"Singles/a.mp3"_s)).has_value());
    EXPECT_EQ(store.count(), 1);
}

TEST_F(InboxStoreTest, StateChangesAndErrors)
{
    const auto id = store.add(single(u"a.mp3"_s));
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(store.setState(*id, InboxItem::State::Failed, u"conversion failed: disk full"_s));
    EXPECT_EQ(store.find(*id)->error, u"conversion failed: disk full"_s);
    ASSERT_TRUE(store.setState(*id, InboxItem::State::Ready));
    EXPECT_TRUE(store.find(*id)->error.isEmpty());
    EXPECT_FALSE(store.setState(9999, InboxItem::State::Ready));
}

TEST_F(InboxStoreTest, UpdateAndRemove)
{
    const auto id = store.add(single(u"a.mp3"_s));
    ASSERT_TRUE(id.has_value());
    InboxItem item = *store.find(*id);
    item.title = u"Neon Rain (Extended Mix)"_s;
    item.state = InboxItem::State::Ready;
    ASSERT_TRUE(store.update(item));
    EXPECT_EQ(store.findByPath(u"a.mp3"_s)->title, u"Neon Rain (Extended Mix)"_s);
    ASSERT_TRUE(store.remove(*id));
    EXPECT_FALSE(store.remove(*id));
    EXPECT_FALSE(store.find(*id).has_value());
}

TEST_F(InboxStoreTest, ListsNewestFirst)
{
    ASSERT_TRUE(store.add(single(u"old.mp3"_s, 1000)).has_value());
    ASSERT_TRUE(store.add(single(u"new.mp3"_s, 5000)).has_value());
    const auto items = store.all();
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items.first().relativePath, u"new.mp3"_s);
}

TEST_F(InboxStoreTest, DatabaseRejectsBadValues)
{
    // The schema's CHECK constraints are a second line of defence behind the C++ types.
    EXPECT_FALSE(db.exec(u"INSERT INTO inbox_items (kind, relative_path, state, created_at) "
                         u"VALUES ('playlist', 'x', 'ready', 1);"));
    EXPECT_FALSE(db.exec(u"INSERT INTO inbox_items (kind, relative_path, state, size_bytes, created_at) "
                         u"VALUES ('single', 'y', 'ready', -5, 1);"));
}
