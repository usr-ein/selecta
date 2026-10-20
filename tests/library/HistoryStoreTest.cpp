// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/HistoryStore.h"

#include "library/InboxStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::library::HistoryEntry;
using selecta::library::HistoryStore;
using Kind = HistoryEntry::Kind;
using Whereabouts = HistoryEntry::Whereabouts;

namespace {

class HistoryStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        session = sessions.start(1000);
        ASSERT_GT(session, 0);
    }

    HistoryEntry download(const QByteArray &remotePath, qint64 completedAt, Kind kind = Kind::Single) const
    {
        HistoryEntry entry;
        entry.session = session;
        entry.completedAt = completedAt;
        entry.kind = kind;
        entry.peer = u"vinyl_ghost"_s;
        entry.remotePath = remotePath;
        entry.sizeBytes = 14'900'000;
        entry.quality = u"FLAC 16/44.1"_s;
        entry.artist = u"Okuma Drift"_s;
        entry.title = u"Neon Rain"_s;
        return entry;
    }

    selecta::storage::Database db;
    selecta::storage::SessionLog sessions{db};
    HistoryStore store{db};
    qint64 session = 0;
};

} // namespace

TEST_F(HistoryStoreTest, RoundTripsAnEntry)
{
    HistoryEntry entry = download("@@vinyl\\Music\\Neon Rain.flac", 2000);
    entry.version = u"Extended Mix"_s;
    const auto id = store.add(entry);
    ASSERT_TRUE(id.has_value()) << db.lastError().toStdString();
    const auto loaded = store.find(*id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->session, session);
    EXPECT_EQ(loaded->remotePath, entry.remotePath);
    EXPECT_EQ(loaded->version, u"Extended Mix"_s);
    EXPECT_EQ(loaded->whereabouts, Whereabouts::Inbox);
    EXPECT_FALSE(loaded->inboxItem.has_value());
    EXPECT_TRUE(loaded->filedPath.isEmpty());
}

TEST_F(HistoryStoreTest, RemotePathsKeepTheirRawBytes)
{
    // Peers send paths in whatever encoding their system uses; history must not mangle them.
    const QByteArray latin1("@@share\\Caf\xe9 del Mar\\track.mp3");
    const auto id = store.add(download(latin1, 2000));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(store.find(*id)->remotePath, latin1);
    EXPECT_TRUE(store.contains(u"vinyl_ghost"_s, latin1));
    EXPECT_FALSE(store.contains(u"someone_else"_s, latin1));
    EXPECT_FALSE(store.contains(u"vinyl_ghost"_s, QByteArray("@@share\\Cafe del Mar\\track.mp3")));
}

TEST_F(HistoryStoreTest, ListsBySessionAndRecency)
{
    ASSERT_TRUE(store.add(download("a", 3000)).has_value());
    ASSERT_TRUE(store.add(download("b", 2000)).has_value());
    const int later = sessions.start(9000);
    ASSERT_GT(later, 0);
    HistoryEntry other = download("c", 9500);
    other.session = later;
    ASSERT_TRUE(store.add(other).has_value());

    const auto first = store.forSession(session);
    ASSERT_EQ(first.size(), 2);
    EXPECT_EQ(first[0].remotePath, QByteArray("b")); // oldest first
    const auto newest = store.recent(2);
    ASSERT_EQ(newest.size(), 2);
    EXPECT_EQ(newest[0].remotePath, QByteArray("c"));
    EXPECT_TRUE(store.recent(0).isEmpty());
}

TEST_F(HistoryStoreTest, DownloadsLeaveOutPreviews)
{
    ASSERT_TRUE(store.add(download("song", 2000)).has_value());
    HistoryEntry preview = download("preview", 2100, Kind::Preview);
    preview.whereabouts = Whereabouts::Preview;
    ASSERT_TRUE(store.add(preview).has_value());
    const auto downloads = store.downloads();
    ASSERT_EQ(downloads.size(), 1);
    EXPECT_EQ(downloads[0].remotePath, QByteArray("song"));
}

TEST_F(HistoryStoreTest, WhereaboutsFollowTheFile)
{
    const auto id = store.add(download("a", 2000));
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(store.setWhereabouts(*id, Whereabouts::Filed, u"/Music/Singles/Okuma Drift - Neon Rain.mp3"_s));
    EXPECT_EQ(store.find(*id)->filedPath, u"/Music/Singles/Okuma Drift - Neon Rain.mp3"_s);
    ASSERT_TRUE(store.setWhereabouts(*id, Whereabouts::Trashed, u"ignored"_s));
    EXPECT_EQ(store.find(*id)->whereabouts, Whereabouts::Trashed);
    EXPECT_TRUE(store.find(*id)->filedPath.isEmpty()); // only filed entries keep a path
    EXPECT_FALSE(store.setWhereabouts(99'999, Whereabouts::Filed));
}

TEST_F(HistoryStoreTest, AnAlbumsEntriesMoveTogetherAndSurviveTheInboxItem)
{
    selecta::library::InboxStore inbox(db);
    selecta::library::InboxItem item;
    item.kind = selecta::library::InboxItem::Kind::Album;
    item.relativePath = u"Albums/Okuma Drift - Nightfall"_s;
    item.createdAt = 2000;
    const auto itemId = inbox.add(item);
    ASSERT_TRUE(itemId.has_value());
    for (const char *path : {"01.flac", "02.flac", "03.flac"}) {
        HistoryEntry entry = download(path, 2000, Kind::Album);
        entry.inboxItem = itemId;
        ASSERT_TRUE(store.add(entry).has_value());
    }
    EXPECT_EQ(store.setWhereaboutsForInboxItem(*itemId, Whereabouts::Filed, u"/Music/Albums/Nightfall"_s), 3);

    // Removing the Inbox item keeps the history but clears the link (ON DELETE SET NULL).
    ASSERT_TRUE(inbox.remove(*itemId));
    const auto entries = store.forSession(session);
    ASSERT_EQ(entries.size(), 3);
    for (const auto &entry : entries) {
        EXPECT_FALSE(entry.inboxItem.has_value());
        EXPECT_EQ(entry.whereabouts, Whereabouts::Filed);
    }
}

TEST_F(HistoryStoreTest, RejectsWhatTheSchemaForbids)
{
    HistoryEntry entry = download("a", 2000);
    entry.session = 12'345; // no such session
    EXPECT_FALSE(store.add(entry).has_value());
    HistoryEntry negative = download("b", 2000);
    negative.sizeBytes = -10; // clamped to 0, not rejected
    const auto id = store.add(negative);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(store.find(*id)->sizeBytes, 0);
}

TEST_F(HistoryStoreTest, TotalsPerPeerWithTheSessionsShare)
{
    ASSERT_TRUE(store.add(download("a", 2000)).has_value());
    ASSERT_TRUE(store.add(download("b", 3000)).has_value());
    HistoryEntry older = download("c", 1000);
    older.session = std::nullopt; // before sessions were recorded
    ASSERT_TRUE(store.add(older).has_value());
    HistoryEntry other = download("d", 2500);
    other.peer = u"aiff_ada"_s;
    other.sizeBytes = 100;
    ASSERT_TRUE(store.add(other).has_value());
    HistoryEntry preview = download("e", 4000, Kind::Preview);
    preview.whereabouts = Whereabouts::Preview;
    ASSERT_TRUE(store.add(preview).has_value());

    const QList<selecta::library::PeerTotals> totals = store.peerTotals(session);
    ASSERT_EQ(totals.size(), 2);
    EXPECT_EQ(totals[0].peer, u"vinyl_ghost"_s); // the most recent first
    EXPECT_EQ(totals[0].files, 3);               // the preview isn't a download
    EXPECT_EQ(totals[0].bytes, 3 * 14'900'000LL);
    EXPECT_EQ(totals[0].sessionFiles, 2);
    EXPECT_EQ(totals[0].lastMs, 3000);
    EXPECT_EQ(totals[1].peer, u"aiff_ada"_s);
    EXPECT_EQ(totals[1].files, 1);
    EXPECT_EQ(totals[1].bytes, 100);
    EXPECT_EQ(store.peerTotals(std::nullopt).constFirst().sessionFiles, 0);
}
