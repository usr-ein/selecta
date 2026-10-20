// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Downloads.h"

#include "storage/Database.h"
#include "storage/Migrations.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::Download;
using selecta::core::DownloadStore;
using State = Download::State;

namespace {

class DownloadsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    static Download sample(const QString &peer, const QByteArray &path, qint64 at = 1000)
    {
        Download download;
        download.peer = peer;
        download.remotePath = path;
        download.size = 12'345'678;
        download.state = State::Waiting;
        download.createdAt = at;
        download.updatedAt = at;
        return download;
    }

    selecta::storage::Database db;
    DownloadStore store{db};
};

} // namespace

TEST_F(DownloadsTest, RoundTripsEveryField)
{
    Download download = sample(u"vinyl_ghost"_s, QByteArray("Music\\Okuma Drift\\01 Neon Rain.flac"));
    selecta::core::FileChoice file;
    file.peer = download.peer;
    file.remotePath = download.remotePath;
    file.size = download.size;
    file.extension = u"flac"_s;
    file.bitDepth = 16;
    file.sampleRate = 44'100;
    download.file = file;
    selecta::meta::Release release;
    release.title = u"Neon Rain"_s;
    release.artists = {u"Okuma Drift"_s};
    release.isrc = u"GBXXX2400001"_s;
    download.release = release;
    download.message = u"Queued at vinyl_ghost"_s;
    download.attempts = 2;
    download.retryAtMs = 5000;
    download.received = 1024;
    download.localPath = u"/tmp/incoming/1/01 Neon Rain.flac.part"_s;
    download.inboxItem = std::nullopt;

    const std::optional<qint64> id = store.add(download);
    ASSERT_TRUE(id.has_value());
    const std::optional<Download> read = store.find(*id);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->peer, download.peer);
    EXPECT_EQ(read->remotePath, download.remotePath);
    EXPECT_EQ(read->size, download.size);
    ASSERT_TRUE(read->file.has_value());
    EXPECT_EQ(read->file->bitDepth, 16);
    ASSERT_TRUE(read->release.has_value());
    EXPECT_EQ(read->release->isrc, u"GBXXX2400001"_s);
    EXPECT_EQ(read->state, State::Waiting);
    EXPECT_EQ(read->message, download.message);
    EXPECT_EQ(read->attempts, 2);
    EXPECT_EQ(read->retryAtMs, 5000);
    EXPECT_EQ(read->received, 1024);
    EXPECT_EQ(read->localPath, download.localPath);
    EXPECT_FALSE(read->getLine.has_value());
    EXPECT_EQ(read->remoteFileName(), u"01 Neon Rain.flac"_s);
}

TEST_F(DownloadsTest, OneUnfinishedDownloadPerFile)
{
    const Download first = sample(u"vinyl_ghost"_s, "a\\b.flac");
    const std::optional<qint64> id = store.add(first);
    ASSERT_TRUE(id.has_value());
    EXPECT_FALSE(store.add(first).has_value());                               // the same file, still unfinished
    EXPECT_TRUE(store.add(sample(u"other_peer"_s, "a\\b.flac")).has_value()); // another peer's copy is another file

    Download finished = *store.find(*id);
    finished.state = State::Failed;
    ASSERT_TRUE(store.update(finished));
    EXPECT_TRUE(store.add(first).has_value()); // once finished, it may be downloaded again
    ASSERT_TRUE(store.findUnfinished(u"vinyl_ghost"_s, "a\\b.flac").has_value());
    EXPECT_NE(store.findUnfinished(u"vinyl_ghost"_s, "a\\b.flac")->id, *id);
}

TEST_F(DownloadsTest, ListsUnfinishedOldestFirstAndByLine)
{
    const qint64 newer = *store.add(sample(u"p"_s, "2.mp3", 2000));
    const qint64 older = *store.add(sample(u"p"_s, "1.mp3", 1000));
    Download done = sample(u"p"_s, "3.mp3", 500);
    done.state = State::Arrived;
    ASSERT_TRUE(store.add(done).has_value());
    const QList<Download> unfinished = store.unfinished();
    ASSERT_EQ(unfinished.size(), 2);
    EXPECT_EQ(unfinished.at(0).id, older);
    EXPECT_EQ(unfinished.at(1).id, newer);
}

TEST_F(DownloadsTest, PrunesOnlyOldFinishedDownloads)
{
    Download old = sample(u"p"_s, "old.mp3", 100);
    old.state = State::Arrived;
    ASSERT_TRUE(store.add(old).has_value());
    Download recent = sample(u"p"_s, "recent.mp3", 9000);
    recent.state = State::Failed;
    ASSERT_TRUE(store.add(recent).has_value());
    ASSERT_TRUE(store.add(sample(u"p"_s, "waiting.mp3", 50)).has_value());
    EXPECT_EQ(store.pruneFinished(1000), 1);
    EXPECT_EQ(store.unfinished().size(), 1);
}

TEST_F(DownloadsTest, StateNamesMatchTheSchema)
{
    for (const State state : {State::Waiting, State::Queued, State::Transferring, State::Converting, State::Arrived,
                              State::Failed, State::Cancelled}) {
        Download download = sample(u"p"_s, selecta::core::downloadStateName(state).toUtf8());
        download.state = state;
        const std::optional<qint64> id = store.add(download);
        ASSERT_TRUE(id.has_value()) << selecta::core::downloadStateName(state).toStdString();
        EXPECT_EQ(store.find(*id)->state, state);
    }
}
