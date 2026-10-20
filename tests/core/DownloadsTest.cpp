// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Downloads.h"

#include "storage/Database.h"
#include "storage/Migrations.h"

#include <gtest/gtest.h>
#include <limits>

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

namespace {

using selecta::core::judgeQueue;
using selecta::core::QueueHistory;
using Kind = selecta::core::QueueVerdict::Kind;

constexpr qint64 kQueued = 1'800'000'000'000; // when the peer queued it (Unix ms)
constexpr qint64 kSecond = 1000;
constexpr qint64 kMinute = 60 * kSecond;

QueueHistory queuedAt(qint64 at)
{
    QueueHistory history;
    history.progressAtMs = at;
    return history;
}

} // namespace

TEST(StuckQueue, TheThresholdsAreTheAnswersConstants)
{
    using namespace std::chrono_literals;
    static_assert(selecta::core::kStuckNoProgress == 2min);
    static_assert(selecta::core::kStuckStartWithin == 10min);
}

TEST(StuckQueue, NoProgressForTwoMinutesIsStuck)
{
    // A peer that never says where we are: nothing has moved since it queued the file.
    const QueueHistory history = queuedAt(kQueued);
    EXPECT_EQ(judgeQueue(history, kQueued).kind, Kind::Waiting);
    EXPECT_EQ(judgeQueue(history, kQueued + (2 * kMinute) - 1).kind, Kind::Waiting);
    const auto stuck = judgeQueue(history, kQueued + (2 * kMinute));
    EXPECT_EQ(stuck.kind, Kind::NoProgress);
    EXPECT_TRUE(stuck.isStuck());
    EXPECT_EQ(stuck.sinceMs, kQueued);
    EXPECT_EQ(stuck.startsAtMs, 0);
    EXPECT_EQ(judgeQueue(history, kQueued + (90 * kMinute)).kind, Kind::NoProgress);
}

TEST(StuckQueue, AnAnswerThatIsntBetterIsNoProgress)
{
    QueueHistory history = queuedAt(kQueued);
    history.sighted(kQueued + (5 * kSecond), 12);
    EXPECT_EQ(judgeQueue(history, kQueued + kMinute).kind, Kind::Waiting);
    EXPECT_EQ(judgeQueue(history, kQueued + (2 * kMinute)).kind, Kind::NoProgress); // no better place seen
    history.sighted(kQueued + (10 * kMinute), 12);                                  // the same place, 10 min on
    const auto stuck = judgeQueue(history, kQueued + (10 * kMinute));
    EXPECT_EQ(stuck.kind, Kind::NoProgress);
    EXPECT_EQ(stuck.sinceMs, kQueued);
    // Worse: privileged users went ahead.
    history.sighted(kQueued + (20 * kMinute), 15);
    EXPECT_EQ(judgeQueue(history, kQueued + (20 * kMinute)).kind, Kind::NoProgress);
}

TEST(StuckQueue, MovingFastEnoughIsntStuck)
{
    // Place 12, then 4 ten minutes later: 0.8 places a minute, so place 4 is 5 minutes from the front.
    QueueHistory history = queuedAt(kQueued);
    history.sighted(kQueued, 12);
    history.sighted(kQueued + (10 * kMinute), 4);
    EXPECT_EQ(history.progressAtMs, kQueued + (10 * kMinute));
    const auto moving = judgeQueue(history, kQueued + (10 * kMinute));
    EXPECT_EQ(moving.kind, Kind::Moving);
    EXPECT_FALSE(moving.isStuck());
    EXPECT_EQ(moving.startsAtMs, kQueued + (15 * kMinute));
    // No answer comes before the next one is due: still moving at that pace.
    EXPECT_EQ(judgeQueue(history, kQueued + (14 * kMinute)).kind, Kind::Moving);
    EXPECT_EQ(judgeQueue(history, kQueued + (17 * kMinute)).kind, Kind::Moving);
}

TEST(StuckQueue, MovingTooSlowlyIsStuck)
{
    // Place 30, then 28 ten minutes later: 0.2 places a minute, 140 minutes to go.
    QueueHistory history = queuedAt(kQueued);
    history.sighted(kQueued, 30);
    history.sighted(kQueued + (10 * kMinute), 28);
    const auto slow = judgeQueue(history, kQueued + (10 * kMinute));
    EXPECT_EQ(slow.kind, Kind::TooSlow);
    EXPECT_TRUE(slow.isStuck());
    EXPECT_EQ(slow.startsAtMs, kQueued + (150 * kMinute));
    // Exactly ten minutes from the front isn't too slow; a little more is.
    QueueHistory edge = queuedAt(kQueued);
    edge.sighted(kQueued, 20);
    edge.sighted(kQueued + (10 * kMinute), 10); // one place a minute: starts at +20 min
    EXPECT_EQ(judgeQueue(edge, kQueued + (10 * kMinute)).kind, Kind::Moving);
    EXPECT_EQ(judgeQueue(edge, kQueued + (10 * kMinute) - 1).kind, Kind::TooSlow);
}

TEST(StuckQueue, LateForItsPaceIsStuck)
{
    // At its pace it was due at +15 min; two minutes past that and still not started.
    QueueHistory history = queuedAt(kQueued);
    history.sighted(kQueued, 12);
    history.sighted(kQueued + (10 * kMinute), 4);
    EXPECT_EQ(judgeQueue(history, kQueued + (17 * kMinute)).kind, Kind::Moving);
    const auto late = judgeQueue(history, kQueued + (17 * kMinute) + 1);
    EXPECT_EQ(late.kind, Kind::NoProgress);
    EXPECT_EQ(late.startsAtMs, kQueued + (15 * kMinute));
    EXPECT_EQ(late.sinceMs, kQueued + (10 * kMinute));
}

TEST(StuckQueue, TheQueueMovingOnRestartsTheClock)
{
    // One of our own files from this peer started: the others moved up. The answers before that tell no pace.
    QueueHistory history = queuedAt(kQueued);
    history.sighted(kQueued, 30);
    history.sighted(kQueued + (10 * kMinute), 28);
    history.progressAtMs = kQueued + (12 * kMinute);
    EXPECT_EQ(judgeQueue(history, kQueued + (13 * kMinute)).kind, Kind::Waiting);
    EXPECT_EQ(judgeQueue(history, kQueued + (14 * kMinute)).kind, Kind::NoProgress);
}

TEST(StuckQueue, OddAnswersStayBounded)
{
    // Two answers at the same moment tell no pace; a huge place from a peer, or a place of 0, can't overflow.
    QueueHistory same = queuedAt(kQueued);
    same.sighted(kQueued + kSecond, 9);
    same.sighted(kQueued + kSecond, 3);
    EXPECT_EQ(judgeQueue(same, kQueued + kMinute).kind, Kind::Waiting);

    QueueHistory huge = queuedAt(kQueued);
    huge.sighted(kQueued, std::numeric_limits<int>::max());
    huge.sighted(kQueued + (10 * kMinute), std::numeric_limits<int>::max() - 1);
    const auto slow = judgeQueue(huge, kQueued + (10 * kMinute));
    EXPECT_EQ(slow.kind, Kind::TooSlow);
    EXPECT_GT(slow.startsAtMs, kQueued);

    QueueHistory next = queuedAt(kQueued);
    next.sighted(kQueued, 1);
    next.sighted(kQueued + (10 * kMinute), 0);
    EXPECT_EQ(judgeQueue(next, kQueued + (10 * kMinute)).kind, Kind::Moving);
    EXPECT_EQ(judgeQueue(next, kQueued + (13 * kMinute)).kind, Kind::NoProgress);

    // A clock that went backwards reads as "too soon to tell".
    EXPECT_EQ(judgeQueue(queuedAt(kQueued), kQueued - kMinute).kind, Kind::Waiting);
}
