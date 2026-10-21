// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TransferService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/GetTracksService.h"
#include "core/NetworkService.h"
#include "core/PreviewCache.h"
#include "core/SearchService.h"
#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"
#include "governor/Governor.h"
#include "library/InboxStore.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::Download;
using selecta::core::DownloadSpec;
using selecta::core::NetworkService;
using selecta::core::TransferService;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;
using State = Download::State;
using Verdict = selecta::core::QueueVerdict::Kind;
using selecta::slsk::peer::PlaceInQueueRequest;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;
constexpr qint64 kMinuteMs = 60'000;

QByteArray content(qsizetype size, quint32 seed)
{
    QRandomGenerator random(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (char &c : bytes) {
        c = static_cast<char>(random.bounded(256));
    }
    return bytes;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class TransferServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(server.start());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        connectNetwork();
        makeService();
    }

    void TearDown() override { stopService(); }

    void connectNetwork()
    {
        network = std::make_unique<NetworkService>(governor, log, tokens, [this] { return now; });
        selecta::core::NetworkConfig config;
        config.host = server.address().toString();
        config.port = server.port();
        config.username = u"selecta_test"_s;
        config.listenPort = 0; // peers reach us through the server
        network->start(config);
        network->setPassword(u"pw"_s);
        ASSERT_TRUE(waitFor([this] { return network->isOnline(); }));
    }

    // A launch: the preview cache opens its folder again (as the engine does), then the service starts.
    void makeService()
    {
        selecta::core::PreviewCache::Options cacheOptions;
        cacheOptions.maxBytes = previewCacheBytes;
        cacheOptions.now = [this] { return now; };
        previewCache = std::make_unique<selecta::core::PreviewCache>(dir.filePath(u"previews"_s), cacheOptions);
        TransferService::Options options;
        options.incomingDir = dir.filePath(u"incoming"_s);
        options.previewCache = previewCache.get();
        options.retryDelaysMs = {100, 100, 100};
        options.pacedRetryMs = 100;
        options.queueCheckMs = 50;
        service = std::make_unique<TransferService>(db, governor, *network, log, options, [this] { return now; });
    }

    // What a quit does: the service lets go of the manager, the network goes, then the service and the cache.
    void stopService()
    {
        if (service) {
            service->shutdown();
        }
        network.reset();
        service.reset();
        previewCache.reset();
    }

    // A quit and a new launch.
    void restart()
    {
        stopService();
        connectNetwork();
        makeService();
        service->load();
    }

    // The files in the preview folder, the cache's index aside.
    QStringList previewFiles() const
    {
        QStringList files;
        QDirIterator it(dir.filePath(u"previews"_s), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.endsWith(u"/index.json"_s)) {
                files.append(path);
            }
        }
        return files;
    }

    std::unique_ptr<FakePeer> peer(const QString &name)
    {
        auto fake = std::make_unique<FakePeer>(name, &server);
        EXPECT_TRUE(fake->start());
        fake->answerIndirect = false; // reachable, so reached directly only: one connection (as slsk's own tests)
        return fake;
    }

    static DownloadSpec spec(const FakePeer &from, const QByteArray &path, qint64 size)
    {
        DownloadSpec result;
        result.peer = from.username();
        result.remotePath = path;
        result.size = size;
        return result;
    }

    bool reaches(qint64 id, State state, std::chrono::milliseconds timeout = 5s)
    {
        return waitFor(
            [&] {
                const std::optional<Download> download = service->download(id);
                return download && download->state == state;
            },
            timeout);
    }

    QTemporaryDir dir;
    FakeServer server;
    qint64 now = kNow;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    std::unique_ptr<NetworkService> network;
    qint64 previewCacheBytes = selecta::core::PreviewCache::kDefaultMaxBytes;
    std::unique_ptr<selecta::core::PreviewCache> previewCache; // outlives the service
    std::unique_ptr<TransferService> service;
};

} // namespace

TEST_F(TransferServiceTest, DownloadsAFileAndHandsItOn)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray path = "Music\\Okuma Drift\\01 Neon Rain.flac";
    const QByteArray bytes = content(300'000, 1);
    ghost->files.insert(path, FakePeer::Upload{.content = bytes});

    QSignalSpy completed(service.get(), &TransferService::completed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, path, bytes.size()));
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }));
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_EQ(done.id, *id);
    EXPECT_EQ(done.state, State::Converting);
    EXPECT_EQ(readFile(done.localPath), bytes);
    EXPECT_FALSE(done.localPath.endsWith(u".part"_s));
    EXPECT_EQ(governor.peerToday(u"vinyl_ghost"_s).files, 1);
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);

    selecta::library::InboxItem item;
    item.relativePath = u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
    item.createdAt = now;
    const std::optional<qint64> inboxItem = selecta::library::InboxStore(db).add(item);
    ASSERT_TRUE(inboxItem.has_value());
    service->arrived(*id, *inboxItem);
    const std::optional<Download> arrived = service->download(*id);
    ASSERT_TRUE(arrived.has_value());
    EXPECT_EQ(arrived->state, State::Arrived);
    EXPECT_EQ(arrived->inboxItem, inboxItem);
    EXPECT_TRUE(service->unfinished().isEmpty());
}

TEST_F(TransferServiceTest, AFileAlreadyWantedIsTheSameDownload)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const QByteArray path = "a\\b.mp3";
    const std::optional<qint64> first = service->enqueue(spec(*ghost, path, 100));
    const std::optional<qint64> second = service->enqueue(spec(*ghost, path, 100));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first, second);
    ASSERT_TRUE(reaches(*first, State::Queued));
    EXPECT_EQ(service->unfinished().size(), 1);
}

TEST_F(TransferServiceTest, AFileThePeerDoesntShareFailsWithTheReason)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->queueBehaviour = FakePeer::QueueBehaviour::Deny;
    ghost->denyReason = u"File not shared."_s;
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "gone.mp3", 100));
    ASSERT_TRUE(reaches(*id, State::Failed));
    const std::optional<Download> failed = service->download(*id);
    EXPECT_TRUE(failed->message.contains(u"File not shared."_s)) << failed->message.toStdString();
    EXPECT_EQ(governor.peerQueued(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, AFullQueueWaitsAndTriesAgain)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->queueBehaviour = FakePeer::QueueBehaviour::Deny;
    ghost->denyReason = u"Too many files"_s;
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "busy.mp3", 100));
    ASSERT_TRUE(reaches(*id, State::Waiting));
    std::optional<Download> waiting = service->download(*id);
    EXPECT_EQ(waiting->attempts, 1);
    ASSERT_TRUE(waiting->retryAtMs.has_value());
    EXPECT_TRUE(waiting->message.contains(u"trying again"_s)) << waiting->message.toStdString();

    // The peer makes room; the retry (once the governor's back-off for the denial is over) gets the file.
    ghost->queueBehaviour = FakePeer::QueueBehaviour::StartUpload;
    ghost->files.insert("busy.mp3", FakePeer::Upload{.content = content(1000, 2)});
    clock.advance(10min);
    now += 600'000;
    QSignalSpy completed(service.get(), &TransferService::completed);
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 10s));
}

TEST_F(TransferServiceTest, ADroppedTransferResumesWhereItStopped)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray path = "big.flac";
    const QByteArray bytes = content(400'000, 3);
    ghost->files.insert(path, FakePeer::Upload{.content = bytes, .stopAfter = 150'000});
    QSignalSpy completed(service.get(), &TransferService::completed);
    QSignalSpy changes(service.get(), &TransferService::changed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, path, bytes.size()));
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 10s));
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_EQ(readFile(done.localPath), bytes); // the second attempt continued from the partial file
    EXPECT_EQ(done.attempts, 1);
    bool waited = false;
    for (const QList<QVariant> &change : std::as_const(changes)) {
        waited = waited || change.at(0).value<Download>().state == State::Waiting;
    }
    EXPECT_TRUE(waited);
    Q_UNUSED(id)
}

TEST_F(TransferServiceTest, CancellingStopsTheTransferAndDeletesTheFile)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray path = "slow.flac";
    ghost->files.insert(path, FakePeer::Upload{.content = content(2'000'000, 4)});
    ghost->uploadRate = 100'000; // 20 s for the whole file
    const std::optional<qint64> id = service->enqueue(spec(*ghost, path, 2'000'000));
    ASSERT_TRUE(reaches(*id, State::Transferring));
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->received > 0; }));
    const QString folder = dir.filePath(u"incoming/%1"_s.arg(*id));
    ASSERT_TRUE(QFileInfo::exists(folder));

    service->cancel(*id);
    EXPECT_EQ(service->download(*id)->state, State::Cancelled);
    EXPECT_FALSE(QFileInfo::exists(folder));
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);
    EXPECT_EQ(governor.peerQueued(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, OfflineDownloadsWaitForTheConnection)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("later.mp3", FakePeer::Upload{.content = content(5000, 5)});
    network->disconnectNow();
    ASSERT_FALSE(network->isOnline());
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "later.mp3", 5000));
    EXPECT_EQ(service->download(*id)->state, State::Waiting);
    EXPECT_TRUE(service->download(*id)->message.contains(u"connection"_s));

    QSignalSpy completed(service.get(), &TransferService::completed);
    network->connectNow();
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 10s));
}

TEST_F(TransferServiceTest, PausedDownloadsStartWhenResumed)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("paused.mp3", FakePeer::Upload{.content = content(5000, 6)});
    governor.setPaused(true);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "paused.mp3", 5000));
    EXPECT_EQ(service->download(*id)->state, State::Waiting);
    QSignalSpy completed(service.get(), &TransferService::completed);
    governor.setPaused(false);
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }));
}

TEST_F(TransferServiceTest, UnfinishedDownloadsComeBackAfterARestart)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "held.flac", 1000));
    ASSERT_TRUE(reaches(*id, State::Queued));

    stopService(); // quit
    connectNetwork();
    makeService();
    service->load();
    ASSERT_TRUE(reaches(*id, State::Queued)); // asked again
    EXPECT_EQ(service->unfinished().size(), 1);

    // The peer's turn comes: the download completes in this new session.
    ghost->files.insert("held.flac", FakePeer::Upload{.content = content(1000, 7)});
    QSignalSpy completed(service.get(), &TransferService::completed);
    ASSERT_TRUE(waitFor([&] { return ghost->heldCount() > 0; }));
    ASSERT_TRUE(ghost->startUpload(u"selecta_test"_s, selecta::slsk::RemotePath(QByteArray("held.flac"))));
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }));
}

TEST_F(TransferServiceTest, AConversionAQuitInterruptedRunsAgain)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("done.mp3", FakePeer::Upload{.content = content(2000, 8)});
    QSignalSpy completed(service.get(), &TransferService::completed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "done.mp3", 2000));
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }));

    stopService(); // quit while converting
    connectNetwork();
    makeService();
    const QSignalSpy again(service.get(), &TransferService::completed);
    service->load();
    ASSERT_EQ(again.size(), 1);
    EXPECT_EQ(again.at(0).at(0).value<Download>().id, *id);
}

TEST_F(TransferServiceTest, APeersNameStaysAsItIsInTheQueueMessages)
{
    // A name with "%2" in it is put in once: the place doesn't land inside it.
    auto holder = peer(u"dj%2%3"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    holder->placeInQueue = 7;
    const std::optional<qint64> id = service->enqueue(spec(*holder, "held.flac", 1000));
    ASSERT_TRUE(reaches(*id, State::Queued));
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->placeInQueue == 7; }));
    EXPECT_EQ(service->download(*id)->message, u"Queued at dj%2%3, place 7"_s);

    now += 2 * kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->queue.isStuck(); }));
    const QString stuck = service->download(*id)->message;
    EXPECT_TRUE(stuck.startsWith(u"No progress in dj%2%3's queue since "_s)) << stuck.toStdString();
    EXPECT_TRUE(stuck.endsWith(u" (place 7)"_s)) << stuck.toStdString();
}

TEST_F(TransferServiceTest, AQueuedDownloadSaysItsPlaceAndIsStuckWithoutProgress)
{
    auto holder = peer(u"slow_lane"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    holder->placeInQueue = 7;
    const std::optional<qint64> id = service->enqueue(spec(*holder, "held.flac", 1000));
    ASSERT_TRUE(reaches(*id, State::Queued));
    // Asked once for its place, through the governor; the answer shows.
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->placeInQueue == 7; }));
    EXPECT_EQ(holder->receivedOf<PlaceInQueueRequest>().size(), 1);
    EXPECT_TRUE(service->download(*id)->message.contains(u"place 7"_s))
        << service->download(*id)->message.toStdString();
    EXPECT_FALSE(service->download(*id)->queue.isStuck());

    // Two minutes on, no better place and no start: stuck, and the message says so plainly.
    now += 2 * kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->queue.isStuck(); }));
    EXPECT_EQ(service->download(*id)->queue.kind, Verdict::NoProgress);
    EXPECT_TRUE(service->download(*id)->message.contains(u"No progress in slow_lane's queue"_s))
        << service->download(*id)->message.toStdString();

    // One question per peer per 10 minutes (the governor's limit): none more before that.
    now += 5 * kMinuteMs;
    clock.advance(7min);
    QTest::qWait(300);
    EXPECT_EQ(holder->receivedOf<PlaceInQueueRequest>().size(), 1);

    // Then asked again. Place 1 after place 7, twelve minutes on: it starts in about two minutes, so it isn't stuck.
    holder->placeInQueue = 1;
    now += 5 * kMinuteMs;
    clock.advance(5min);
    ASSERT_TRUE(waitFor([&] { return holder->receivedOf<PlaceInQueueRequest>().size() == 2; }));
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->placeInQueue == 1; }));
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->queue.kind == Verdict::Moving; }));
    EXPECT_FALSE(service->download(*id)->queue.isStuck());
    EXPECT_TRUE(service->download(*id)->message.contains(u"starts around"_s))
        << service->download(*id)->message.toStdString();

    // Its turn: transferring, it is in nobody's queue any more.
    holder->files.insert("held.flac", FakePeer::Upload{.content = content(1000, 20)});
    QSignalSpy completed(service.get(), &TransferService::completed);
    ASSERT_TRUE(holder->startUpload(u"selecta_test"_s, selecta::slsk::RemotePath(QByteArray("held.flac"))));
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }));
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_FALSE(done.placeInQueue.has_value());
    EXPECT_EQ(done.queue.kind, Verdict::Waiting);
}

TEST_F(TransferServiceTest, APeerThatNeverSaysItsQueueIsStuckAfterTwoMinutes)
{
    auto silent = peer(u"quiet_one"_s);
    silent->queueBehaviour = FakePeer::QueueBehaviour::Hold; // and no placeInQueue: it never answers
    const std::optional<qint64> id = service->enqueue(spec(*silent, "held.flac", 1000));
    ASSERT_TRUE(reaches(*id, State::Queued));
    ASSERT_TRUE(waitFor([&] { return silent->receivedOf<PlaceInQueueRequest>().size() == 1; }));
    now += (2 * kMinuteMs) - 1000;
    QTest::qWait(200);
    EXPECT_FALSE(service->download(*id)->queue.isStuck());
    now += 1000;
    ASSERT_TRUE(waitFor([&] { return service->download(*id)->queue.isStuck(); }));
    EXPECT_FALSE(service->download(*id)->placeInQueue.has_value());
    EXPECT_TRUE(service->download(*id)->message.contains(u"No progress in quiet_one's queue since"_s))
        << service->download(*id)->message.toStdString();
}

TEST_F(TransferServiceTest, FilesBehindOurOwnTransferJustWaitTheirTurn)
{
    auto holder = peer(u"one_at_a_time"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    holder->files.insert("first.flac", FakePeer::Upload{.content = content(2'000'000, 21)});
    holder->uploadRate = 100'000; // 20 s for the whole file
    const std::optional<qint64> first = service->enqueue(spec(*holder, "first.flac", 2'000'000));
    const std::optional<qint64> second = service->enqueue(spec(*holder, "second.flac", 1000));
    ASSERT_TRUE(reaches(*first, State::Queued));
    ASSERT_TRUE(reaches(*second, State::Queued));
    ASSERT_TRUE(waitFor([&] { return holder->heldCount() == 2; }));
    ASSERT_TRUE(holder->startUpload(u"selecta_test"_s, selecta::slsk::RemotePath(QByteArray("first.flac"))));
    ASSERT_TRUE(reaches(*first, State::Transferring));

    // The peer sends us one file at a time: the second waits behind the first, however long that takes.
    now += 5 * kMinuteMs;
    QTest::qWait(300);
    EXPECT_FALSE(service->download(*second)->queue.isStuck());

    // The first ends: from then on, the second has two minutes.
    service->cancel(*first);
    now += kMinuteMs;
    QTest::qWait(200);
    EXPECT_FALSE(service->download(*second)->queue.isStuck());
    now += kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return service->download(*second)->queue.isStuck(); }));
}

namespace {

// Searches go nowhere; the test delivers results itself (SearchService::received).
struct LocalSearch final : selecta::core::SearchNetwork
{
    int sent = 0;
    bool isOnline() const override { return true; }
    quint32 search([[maybe_unused]] const QString &query,
                   [[maybe_unused]] selecta::governor::Permit permit) override // clazy:exclude=function-args-by-ref
    {
        return static_cast<quint32>(++sent);
    }
    void stopSearch([[maybe_unused]] quint32 token) override {}
};

selecta::slsk::peer::FileSearchResponse result(quint32 token, const QString &path, quint64 size)
{
    selecta::slsk::peer::FileSearchResponse response;
    response.token = token;
    selecta::slsk::FileEntry file;
    file.name = selecta::slsk::RemotePath::fromText(path);
    file.size = size;
    file.attributes.bitrate = 320;
    file.attributes.duration = 372;
    response.results.append(file);
    response.slotFree = true;
    return response;
}

} // namespace

// P-010: the user asked the Collection view for a better copy of a track; one found with confidence comes by itself,
// a better one only.
TEST_F(TransferServiceTest, AConfidentUpgradeDownloadsByItself)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QString ghostPath = u"@@g\\Okuma Drift - Neon Rain.mp3"_s;
    constexpr qint64 kSize = 14'880'000; // 320 kbps × 372 s
    ghost->files.insert(selecta::slsk::RemotePath::fromText(ghostPath).raw,
                        FakePeer::Upload{.content = content(kSize, 3)});

    LocalSearch network;
    selecta::core::SearchService search(governor, network, clock, {.idle = 40ms, .total = 1000ms});
    selecta::core::GetTracksService getTracks(db, [this] { return now; });
    getTracks.setSearch(&search, [] { return QStringList(); });
    getTracks.setTransfers(service.get());
    getTracks.setTierLookup([](const QString &) { return std::optional(selecta::core::QualityTier::Below192); });
    QObject::connect(service.get(), &TransferService::changed, &getTracks,
                     &selecta::core::GetTracksService::downloadChanged);
    QSignalSpy completed(service.get(), &TransferService::completed);
    const auto added = getTracks.addUpgrades({{.path = u"/Music/Tracks/Okuma Drift - Neon Rain.mp3"_s,
                                               .artist = u"Okuma Drift"_s,
                                               .title = u"Neon Rain"_s,
                                               .durationMs = 372'000}});
    ASSERT_EQ(added.lines.size(), 1);
    ASSERT_TRUE(waitFor([&] { return network.sent == 1; }));
    search.received(1, u"vinyl_ghost"_s, result(1, ghostPath, kSize));
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 15s)); // nobody pressed Download
    EXPECT_EQ(completed.at(0).at(0).value<Download>().getLine, added.lines.constFirst());
    const selecta::core::GetLine line = getTracks.lines().constFirst();
    EXPECT_EQ(line.findState, selecta::core::GetLine::FindState::Ready);
    EXPECT_EQ(line.downloadState, selecta::core::GetLine::DownloadState::Converting);
}

TEST_F(TransferServiceTest, GetTracksMovesOnToTheNextPeersCopy)
{
    auto rita = peer(u"refusing_rita"_s);
    auto ghost = peer(u"vinyl_ghost"_s);
    const QString ritaPath = u"@@r\\Okuma Drift - Neon Rain.mp3"_s;
    const QString ghostPath = u"@@g\\Okuma Drift - Neon Rain.mp3"_s;
    constexpr qint64 kSize = 14'880'000; // 320 kbps × 372 s

    LocalSearch network;
    selecta::core::SearchService search(governor, network, clock, {.idle = 40ms, .total = 1000ms});
    selecta::core::GetTracksService getTracks(db, [this] { return now; });
    getTracks.setSearch(&search, [] { return QStringList(); });
    getTracks.setTransfers(service.get());
    QObject::connect(service.get(), &TransferService::changed, &getTracks,
                     &selecta::core::GetTracksService::downloadChanged);
    selecta::meta::Release release;
    release.title = u"Neon Rain"_s;
    release.artists = {u"Okuma Drift"_s};
    release.durationMs = 372'000;
    getTracks.addReleases({release});
    getTracks.find();
    ASSERT_TRUE(waitFor([&] { return network.sent == 1; }));
    search.received(1, u"refusing_rita"_s, result(1, ritaPath, kSize));
    search.received(1, u"vinyl_ghost"_s, result(1, ghostPath, kSize));
    ASSERT_TRUE(waitFor([&] { return getTracks.findingCount() == 0; }));
    selecta::core::GetLine line = getTracks.lines().constFirst();
    ASSERT_TRUE(line.best.has_value());
    ASSERT_EQ(line.alternatives.size(), 1);

    // Whichever copy ranked first is refused; the other one is shared.
    const bool ritaFirst = line.best->peer == u"refusing_rita"_s;
    FakePeer &refuser = ritaFirst ? *rita : *ghost;
    FakePeer &sharer = ritaFirst ? *ghost : *rita;
    refuser.queueBehaviour = FakePeer::QueueBehaviour::Deny;
    refuser.denyReason = u"File not shared."_s;
    sharer.files.insert(selecta::slsk::RemotePath::fromText(ritaFirst ? ghostPath : ritaPath).raw,
                        FakePeer::Upload{.content = content(kSize, 9)});

    QSignalSpy completed(service.get(), &TransferService::completed);
    getTracks.download({line.id});
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 15s));
    EXPECT_EQ(completed.at(0).at(0).value<Download>().peer, sharer.username());
    line = getTracks.lines().constFirst();
    EXPECT_EQ(line.downloadState, selecta::core::GetLine::DownloadState::Converting);
    EXPECT_EQ(line.best->peer, sharer.username());
}

namespace {

// A search result with a FLAC file, or an MP3 at another bitrate.
selecta::slsk::peer::FileSearchResponse flacResult(quint32 token, const QString &path, quint64 size)
{
    selecta::slsk::peer::FileSearchResponse response = result(token, path, size);
    response.results[0].attributes.bitrate.reset();
    response.results[0].attributes.sampleRate = 44'100;
    response.results[0].attributes.bitDepth = 16;
    return response;
}

selecta::slsk::peer::FileSearchResponse mp3Result(quint32 token, const QString &path, quint32 bitrate)
{
    selecta::slsk::peer::FileSearchResponse response = result(token, path, quint64{bitrate} * 1000 / 8 * 372);
    response.results[0].attributes.bitrate = bitrate;
    return response;
}

// Get tracks on top of the service under test, finding through searches the test answers.
struct GetTracksRig
{
    GetTracksRig(TransferService &transfers, selecta::governor::Governor &governor,
                 const selecta::base::ManualClock &clock, selecta::storage::Database &db,
                 const std::function<qint64()> &now)
        : search(governor, network, clock, {.idle = 40ms, .total = 1000ms})
        , getTracks(db, now)
    {
        getTracks.setSearch(&search, [] { return QStringList(); });
        getTracks.setTransfers(&transfers);
        QObject::connect(&transfers, &TransferService::changed, &getTracks,
                         &selecta::core::GetTracksService::downloadChanged);
    }

    // Adds Okuma Drift's "Neon Rain" and starts finding it.
    void addTrack()
    {
        selecta::meta::Release release;
        release.title = u"Neon Rain"_s;
        release.artists = {u"Okuma Drift"_s};
        release.durationMs = 372'000;
        getTracks.addReleases({release});
        getTracks.find();
    }

    selecta::core::GetLine line() const { return getTracks.lines().constFirst(); }

    LocalSearch network;
    selecta::core::SearchService search;
    selecta::core::GetTracksService getTracks;
};

} // namespace

TEST_F(TransferServiceTest, TryingAnotherPeerLeavesTheOldQueueAndGetsTheOtherCopy)
{
    auto holder = peer(u"forever_queue"_s);
    auto sharer = peer(u"free_slot"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const QString holderPath = u"@@h\\Okuma Drift - Neon Rain.flac"_s;
    const QString sharerPath = u"@@s\\Okuma Drift - Neon Rain.mp3"_s;
    constexpr qint64 kSize = 14'880'000; // 320 kbps × 372 s
    sharer->files.insert(selecta::slsk::RemotePath::fromText(sharerPath).raw,
                         FakePeer::Upload{.content = content(kSize, 22)});

    GetTracksRig rig(*service, governor, clock, db, [this] { return now; });
    rig.addTrack();
    ASSERT_TRUE(waitFor([&] { return rig.network.sent == 1; }));
    rig.search.received(1, holder->username(), flacResult(1, holderPath, 42'000'000));
    rig.search.received(1, sharer->username(), result(1, sharerPath, kSize));
    ASSERT_TRUE(waitFor([&] { return rig.getTracks.findingCount() == 0; }));
    selecta::core::GetLine line = rig.line();
    ASSERT_EQ(line.best->peer, holder->username()); // lossless first
    ASSERT_EQ(line.alternatives.size(), 1);

    rig.getTracks.download({line.id});
    ASSERT_TRUE(waitFor([&] { return holder->heldCount() == 1; }));
    ASSERT_TRUE(waitFor([&] { return rig.line().downloadState == selecta::core::GetLine::DownloadState::Queued; }));
    const qint64 queuedAtHolder = rig.line().downloadId;
    EXPECT_FALSE(rig.line().stuck);
    // Stuck two minutes on: the line says so.
    now += 2 * kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return rig.line().stuck; }));

    // Try another peer: the other copy is asked for, and the queue place at the holder given back.
    QSignalSpy completed(service.get(), &TransferService::completed);
    rig.getTracks.switchPeer(line.id);
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 15s));
    EXPECT_EQ(completed.at(0).at(0).value<Download>().peer, sharer->username());
    line = rig.line();
    EXPECT_EQ(line.best->peer, sharer->username());
    EXPECT_TRUE(line.leftPeers.contains(holder->username()));
    EXPECT_FALSE(line.stuck);
    EXPECT_EQ(service->download(queuedAtHolder)->state, State::Cancelled);
    EXPECT_EQ(governor.peerQueued(holder->username()), 0);

    // There is no message to leave a queue: when the holder's turn comes, it is answered "Cancelled" and moves on.
    ASSERT_TRUE(holder->startUpload(u"selecta_test"_s, selecta::slsk::RemotePath::fromText(holderPath)));
    ASSERT_TRUE(waitFor([&] { return !holder->receivedOf<selecta::slsk::peer::TransferResponse>().isEmpty(); }));
    const auto answer = holder->receivedOf<selecta::slsk::peer::TransferResponse>().constFirst();
    EXPECT_FALSE(answer.allowed);
    EXPECT_EQ(answer.reason, u"Cancelled"_s);

    // The holder is never offered again for this line.
    rig.getTracks.switchPeer(line.id);
    EXPECT_EQ(rig.line().best->peer, sharer->username());
}

TEST_F(TransferServiceTest, TryingAnotherPeerAfterARestartFindsTheOtherCopiesAgain)
{
    auto holder = peer(u"forever_queue"_s);
    auto sharer = peer(u"free_slot"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const QString holderPath = u"@@h\\Okuma Drift - Neon Rain.flac"_s;
    const QString sharerPath = u"@@s\\Okuma Drift - Neon Rain.mp3"_s;
    constexpr qint64 kSize = 14'880'000;
    sharer->files.insert(selecta::slsk::RemotePath::fromText(sharerPath).raw,
                         FakePeer::Upload{.content = content(kSize, 23)});

    auto rig = std::make_unique<GetTracksRig>(*service, governor, clock, db, [this] { return now; });
    rig->addTrack();
    ASSERT_TRUE(waitFor([&] { return rig->network.sent == 1; }));
    rig->search.received(1, holder->username(), flacResult(1, holderPath, 42'000'000));
    rig->search.received(1, sharer->username(), result(1, sharerPath, kSize));
    ASSERT_TRUE(waitFor([&] { return rig->getTracks.findingCount() == 0; }));
    const qint64 id = rig->line().id;
    rig->getTracks.download({id});
    ASSERT_TRUE(waitFor([&] { return holder->heldCount() == 1; }));

    // Get tracks starts again (the search service and its cache stay): the line knows no other copy.
    selecta::core::GetTracksService restarted(db, [this] { return now; });
    restarted.setSearch(&rig->search, [] { return QStringList(); });
    restarted.setTransfers(service.get());
    QObject::disconnect(service.get(), &TransferService::changed, &rig->getTracks, nullptr);
    QObject::connect(service.get(), &TransferService::changed, &restarted,
                     &selecta::core::GetTracksService::downloadChanged);
    ASSERT_TRUE(restarted.lines().constFirst().alternatives.isEmpty());
    now += 2 * kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return restarted.lines().constFirst().stuck; }));

    // Try another peer: found again (the cache answers, no search goes out), then the other copy is got.
    QSignalSpy completed(service.get(), &TransferService::completed);
    restarted.switchPeer(id);
    EXPECT_TRUE(restarted.lines().constFirst().lookingForPeers);
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 15s));
    EXPECT_EQ(completed.at(0).at(0).value<Download>().peer, sharer->username());
    EXPECT_EQ(rig->network.sent, 1);
    EXPECT_FALSE(restarted.lines().constFirst().lookingForPeers);
    EXPECT_EQ(governor.peerQueued(holder->username()), 0);
}

TEST_F(TransferServiceTest, NoOtherPeerLeavesTheLineWhereItWaits)
{
    auto holder = peer(u"forever_queue"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const QString holderPath = u"@@h\\Okuma Drift - Neon Rain.flac"_s;
    GetTracksRig rig(*service, governor, clock, db, [this] { return now; });
    rig.addTrack();
    ASSERT_TRUE(waitFor([&] { return rig.network.sent == 1; }));
    rig.search.received(1, holder->username(), flacResult(1, holderPath, 42'000'000));
    ASSERT_TRUE(waitFor([&] { return rig.getTracks.findingCount() == 0; }));
    const qint64 id = rig.line().id;
    rig.getTracks.download({id});
    ASSERT_TRUE(waitFor([&] { return holder->heldCount() == 1; }));
    const qint64 waiting = rig.line().downloadId;

    rig.getTracks.switchPeer(id);
    ASSERT_TRUE(waitFor([&] { return !rig.line().lookingForPeers; }));
    EXPECT_EQ(rig.line().peersNote, u"No other peer has it in the quality wanted."_s);
    EXPECT_EQ(rig.line().downloadId, waiting);
    EXPECT_EQ(service->download(waiting)->state, State::Queued);
    EXPECT_EQ(rig.line().best->peer, holder->username());
}

TEST_F(TransferServiceTest, AnOtherCopyThatCantBeAskedForLeavesTheLineAsItWas)
{
    auto holder = peer(u"forever_queue"_s);
    auto sharer = peer(u"free_slot"_s);
    holder->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const QString holderPath = u"@@h\\Okuma Drift - Neon Rain.flac"_s;
    const QString sharerPath = u"@@s\\Okuma Drift - Neon Rain.mp3"_s;
    GetTracksRig rig(*service, governor, clock, db, [this] { return now; });
    rig.addTrack();
    ASSERT_TRUE(waitFor([&] { return rig.network.sent == 1; }));
    rig.search.received(1, holder->username(), flacResult(1, holderPath, 42'000'000));
    rig.search.received(1, sharer->username(), result(1, sharerPath, 14'880'000));
    ASSERT_TRUE(waitFor([&] { return rig.getTracks.findingCount() == 0; }));
    const qint64 id = rig.line().id;
    rig.getTracks.download({id});
    ASSERT_TRUE(waitFor([&] { return holder->heldCount() == 1; }));
    now += 2 * kMinuteMs;
    ASSERT_TRUE(waitFor([&] { return rig.line().stuck; }));
    const selecta::core::GetLine stuck = rig.line();

    // The database turns the other copy's download down: the line keeps waiting where it was, still stuck (the
    // transfers wouldn't say it again), and says why; the other copy stays to try.
    ASSERT_TRUE(db.exec(u"CREATE TRIGGER refuse_downloads BEFORE INSERT ON downloads "
                        "BEGIN SELECT RAISE(ABORT, 'disk full'); END;"));
    rig.getTracks.switchPeer(id);
    const selecta::core::GetLine line = rig.line();
    EXPECT_EQ(line.best, stuck.best);
    EXPECT_EQ(line.downloadId, stuck.downloadId);
    EXPECT_EQ(line.downloadState, selecta::core::GetLine::DownloadState::Queued);
    EXPECT_EQ(line.findState, stuck.findState);
    EXPECT_TRUE(line.stuck);
    EXPECT_EQ(line.message, stuck.message);
    EXPECT_EQ(line.peersNote, u"Selecta couldn't ask %1 for the file."_s.arg(sharer->username()));
    EXPECT_EQ(line.alternatives, stuck.alternatives);
    EXPECT_TRUE(line.leftPeers.isEmpty());
    EXPECT_EQ(service->download(stuck.downloadId)->state, State::Queued); // its place in the queue is kept
    EXPECT_EQ(governor.peerQueued(holder->username()), 1);
    EXPECT_EQ(governor.peerQueued(sharer->username()), 0);
}

TEST_F(TransferServiceTest, GettingANearMissAnywayDownloadsItBelowTheQualityWanted)
{
    auto low = peer(u"low_bitrate"_s);
    auto lower = peer(u"lower_bitrate"_s);
    const QString lowPath = u"@@l\\Okuma Drift - Neon Rain.mp3"_s;
    const QString lowerPath = u"@@m\\Okuma Drift - Neon Rain.mp3"_s;
    constexpr qint64 kSize = qint64{192} * 1000 / 8 * 372;
    lower->files.insert(selecta::slsk::RemotePath::fromText(lowerPath).raw,
                        FakePeer::Upload{.content = content(kSize / 2, 24)});

    GetTracksRig rig(*service, governor, clock, db, [this] { return now; });
    rig.addTrack();
    ASSERT_TRUE(waitFor([&] { return rig.network.sent == 1; }));
    rig.search.received(1, low->username(), mp3Result(1, lowPath, 192));
    rig.search.received(1, lower->username(), mp3Result(1, lowerPath, 96));
    ASSERT_TRUE(waitFor([&] { return rig.getTracks.findingCount() == 0; }));
    selecta::core::GetLine line = rig.line();
    ASSERT_EQ(line.findState, selecta::core::GetLine::FindState::NotFound);
    ASSERT_EQ(line.nearMisses.size(), 2);
    EXPECT_EQ(line.nearMisses[0].peer, low->username());
    EXPECT_EQ(line.message, u"2 files of this track found, below the quality wanted."_s);

    // The user picks the second (radio buttons): exactly that file is got, not spread elsewhere.
    QSignalSpy completed(service.get(), &TransferService::completed);
    rig.getTracks.getAnyway(line.id, line.nearMisses[1].peer, line.nearMisses[1].remotePath);
    ASSERT_TRUE(waitFor([&] { return !completed.isEmpty(); }, 15s));
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_EQ(done.peer, lower->username());
    ASSERT_TRUE(done.file.has_value());
    EXPECT_EQ(done.file->shortfall, u"under 320 kbps"_s); // the Inbox marks it below the quality wanted
    line = rig.line();
    EXPECT_EQ(line.findState, selecta::core::GetLine::FindState::Check);
    EXPECT_TRUE(line.nearMisses.isEmpty());
    ASSERT_EQ(line.alternatives.size(), 1); // the other near miss, should this one fail
    EXPECT_EQ(line.alternatives[0].peer, low->username());

    // A near miss the line doesn't have does nothing.
    rig.getTracks.getAnyway(line.id, u"nobody"_s, "x.mp3");
    EXPECT_EQ(rig.line().best->peer, lower->username());
}

TEST_F(TransferServiceTest, APreviewPlaysTheFileWhileItArrives)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(600'000, 21);
    ghost->files.insert("preview.flac", FakePeer::Upload{.content = bytes});
    ghost->uploadRate = 400'000; // about 1.5 s for the whole file
    QString why;
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "preview.flac", bytes.size()), &why);
    ASSERT_TRUE(started.has_value()) << why.toStdString();
    ASSERT_TRUE(started->source);
    EXPECT_EQ(started->source->totalSize(), bytes.size());
    // Readable as it grows…
    ASSERT_TRUE(waitFor([&] {
        const qint64 available = started->source->available();
        return available > 0 && available < bytes.size();
    }));
    QByteArray head(1000, '\0');
    ASSERT_EQ(started->source->read(0, head.data(), head.size()), head.size());
    EXPECT_EQ(head, bytes.left(1000));
    // …and complete at the end.
    ASSERT_TRUE(waitFor([&] { return started->source->snapshot().isComplete(); }, 10s));
    ASSERT_TRUE(service->preview(started->id).has_value());
    EXPECT_EQ(service->preview(started->id)->state, selecta::core::Preview::State::Complete);
    EXPECT_EQ(governor.peerToday(u"vinyl_ghost"_s).files, 1); // previews count towards the peer's totals
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, GettingAFilePreviewedToTheEndDoesntDownloadItAgain)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(50'000, 22);
    ghost->files.insert("again.mp3", FakePeer::Upload{.content = bytes});
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "again.mp3", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->snapshot().isComplete(); }));

    const QSignalSpy completed(service.get(), &TransferService::completed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "again.mp3", bytes.size()));
    ASSERT_TRUE(id.has_value());
    ASSERT_EQ(completed.size(), 1); // at once: no second transfer
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_EQ(readFile(done.localPath), bytes);
    EXPECT_EQ(governor.peerToday(u"vinyl_ghost"_s).files, 1);
}

TEST_F(TransferServiceTest, StoppingAPreviewCancelsItAndFreesThePeer)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("long.flac", FakePeer::Upload{.content = content(2'000'000, 23)});
    ghost->uploadRate = 100'000; // 20 s
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "long.flac", 2'000'000));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));
    service->stopPreview(started->id);
    EXPECT_EQ(service->preview(started->id)->state, selecta::core::Preview::State::Cancelled);
    EXPECT_EQ(started->source->snapshot().state, selecta::audio::SourceState::Failed);
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);
    EXPECT_EQ(governor.peerQueued(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, ANewPreviewReplacesTheOneThatWasPlaying)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    auto lena = peer(u"lossless_lena"_s);
    ghost->files.insert("first.flac", FakePeer::Upload{.content = content(2'000'000, 24)});
    ghost->uploadRate = 100'000;
    lena->files.insert("second.flac", FakePeer::Upload{.content = content(20'000, 25)});
    const std::optional<TransferService::PreviewStart> first =
        service->startPreview(spec(*ghost, "first.flac", 2'000'000));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(waitFor([&] { return first->source->available() > 0; }));
    // The same file again is the same preview.
    EXPECT_EQ(service->startPreview(spec(*ghost, "first.flac", 2'000'000))->id, first->id);

    const std::optional<TransferService::PreviewStart> second =
        service->startPreview(spec(*lena, "second.flac", 20'000));
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(second->id, first->id);
    EXPECT_FALSE(service->preview(first->id).has_value()); // stopped, then forgotten
    ASSERT_TRUE(waitFor([&] { return second->source->snapshot().isComplete(); }));
}

TEST_F(TransferServiceTest, APreviewNeedsTheConnectionAndAKnownSize)
{
    const auto ghost = peer(u"vinyl_ghost"_s);
    QString why;
    EXPECT_FALSE(service->startPreview(spec(*ghost, "unknown.mp3", 0), &why).has_value());
    EXPECT_TRUE(why.contains(u"how big"_s)) << why.toStdString();
    network->disconnectNow();
    EXPECT_FALSE(service->startPreview(spec(*ghost, "offline.mp3", 100), &why).has_value());
    EXPECT_TRUE(why.contains(u"Not connected"_s)) << why.toStdString();
}

TEST_F(TransferServiceTest, KeepingAPreviewStillArrivingMakesItTheDownload)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    auto lena = peer(u"lossless_lena"_s);
    const QByteArray bytes = content(400'000, 26);
    ghost->files.insert("keep.flac", FakePeer::Upload{.content = bytes});
    ghost->uploadRate = 200'000; // 2 s
    lena->files.insert("next.flac", FakePeer::Upload{.content = content(20'000, 27)});
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "keep.flac", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));

    const QSignalSpy completed(service.get(), &TransferService::completed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "keep.flac", bytes.size()));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(service->download(*id)->state, State::Waiting);
    EXPECT_TRUE(completed.isEmpty());
    // Skipping to another preview no longer cancels the kept one.
    ASSERT_TRUE(service->startPreview(spec(*lena, "next.flac", 20'000)).has_value());

    ASSERT_TRUE(waitFor([&] { return completed.size() == 1; }, 10s));
    const auto done = completed.at(0).at(0).value<Download>();
    EXPECT_EQ(done.id, *id);
    EXPECT_EQ(readFile(done.localPath), bytes);
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 1); // one transfer, not two
}

TEST_F(TransferServiceTest, AKeptPreviewThatFailsDownloadsTheUsualWay)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(400'000, 28);
    ghost->files.insert("flaky.flac", FakePeer::Upload{.content = bytes, .stopAfter = 100'000});
    ghost->uploadRate = 200'000;
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "flaky.flac", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "flaky.flac", bytes.size()));
    ASSERT_TRUE(id.has_value());
    // The preview breaks off (once); the download then asks the peer itself.
    EXPECT_TRUE(reaches(*id, State::Converting, 20s));
}

TEST_F(TransferServiceTest, APreviewHeardBeforePlaysFromTheCacheAfterARestart)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(80'000, 30);
    ghost->files.insert("heard.flac", FakePeer::Upload{.content = bytes});
    const std::optional<TransferService::PreviewStart> first =
        service->startPreview(spec(*ghost, "heard.flac", bytes.size()));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(waitFor([&] { return first->source->snapshot().isComplete(); }));

    restart();
    QString why;
    const std::optional<TransferService::PreviewStart> again =
        service->startPreview(spec(*ghost, "heard.flac", bytes.size()), &why);
    ASSERT_TRUE(again.has_value()) << why.toStdString();
    // At once, complete, from the cache: the peer isn't asked a second time.
    ASSERT_TRUE(again->source->snapshot().isComplete());
    QByteArray all(bytes.size(), '\0');
    ASSERT_EQ(again->source->read(0, all.data(), all.size()), bytes.size());
    EXPECT_EQ(all, bytes);
    EXPECT_EQ(service->preview(again->id)->state, selecta::core::Preview::State::Complete);
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 1);
    EXPECT_EQ(governor.peerToday(u"vinyl_ghost"_s).files, 1);
}

TEST_F(TransferServiceTest, AGetAfterARestartTakesThePreviewHeardBefore)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(50'000, 31);
    ghost->files.insert("kept.mp3", FakePeer::Upload{.content = bytes});
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "kept.mp3", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->snapshot().isComplete(); }));

    restart();
    const QSignalSpy completed(service.get(), &TransferService::completed);
    ASSERT_TRUE(service->enqueue(spec(*ghost, "kept.mp3", bytes.size())).has_value());
    ASSERT_EQ(completed.size(), 1); // at once: no second transfer
    EXPECT_EQ(readFile(completed.at(0).at(0).value<Download>().localPath), bytes);
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 1);
}

TEST_F(TransferServiceTest, APreviewAQuitCutShortIsntKept)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("cut.flac", FakePeer::Upload{.content = content(2'000'000, 32)});
    ghost->uploadRate = 100'000; // 20 s
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "cut.flac", 2'000'000));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));
    ASSERT_EQ(previewFiles().size(), 1);

    restart(); // the half-written file goes; nothing claims to have it
    EXPECT_TRUE(previewFiles().isEmpty());
    ghost->uploadRate = 0;
    QString why;
    const std::optional<TransferService::PreviewStart> again =
        service->startPreview(spec(*ghost, "cut.flac", 2'000'000), &why);
    ASSERT_TRUE(again.has_value()) << why.toStdString();
    EXPECT_FALSE(again->source->snapshot().isComplete()); // asked for again
    ASSERT_TRUE(waitFor([&] { return again->source->snapshot().isComplete(); }, 10s));
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 2);
}

TEST_F(TransferServiceTest, ACachedPreviewOfAnotherSizeIsAskedForAgain)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("changed.flac", FakePeer::Upload{.content = content(30'000, 33)});
    const std::optional<TransferService::PreviewStart> first =
        service->startPreview(spec(*ghost, "changed.flac", 30'000));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(waitFor([&] { return first->source->snapshot().isComplete(); }));

    // The peer replaced the file since: the copy heard before is of no use.
    const QByteArray replaced = content(40'000, 34);
    ghost->files.insert("changed.flac", FakePeer::Upload{.content = replaced});
    restart();
    const std::optional<TransferService::PreviewStart> again =
        service->startPreview(spec(*ghost, "changed.flac", replaced.size()));
    ASSERT_TRUE(again.has_value());
    ASSERT_TRUE(waitFor([&] { return again->source->snapshot().isComplete(); }));
    EXPECT_EQ(again->source->totalSize(), replaced.size());
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 2);
    EXPECT_EQ(previewFiles().size(), 1); // the old copy went
}

TEST_F(TransferServiceTest, TheCacheKeepsTheLatestPreviewsWithinItsBound)
{
    previewCacheBytes = 100'000;
    stopService();
    connectNetwork();
    makeService();
    auto ghost = peer(u"vinyl_ghost"_s);
    for (const QByteArray &name : {QByteArray("one.flac"), QByteArray("two.flac"), QByteArray("three.flac")}) {
        ghost->files.insert(name, FakePeer::Upload{.content = content(40'000, 35)});
        const std::optional<TransferService::PreviewStart> started = service->startPreview(spec(*ghost, name, 40'000));
        ASSERT_TRUE(started.has_value());
        ASSERT_TRUE(waitFor([&] { return started->source->snapshot().isComplete(); }));
        now += 1000; // heard one after the other
    }
    // 120 000 bytes in a 100 000-byte cache: the least recently heard went.
    const QStringList files = previewFiles();
    ASSERT_EQ(files.size(), 2);
    EXPECT_FALSE(files.join(u'\n').contains(u"one.flac"_s));

    const std::optional<TransferService::PreviewStart> latest =
        service->startPreview(spec(*ghost, "three.flac", 40'000));
    ASSERT_TRUE(latest.has_value());
    EXPECT_TRUE(latest->source->snapshot().isComplete()); // still there: from the cache
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 3);
}

TEST_F(TransferServiceTest, DiscardingAPreviewStillArrivingCancelsItAndDeletesWhatArrived)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("unwanted.flac", FakePeer::Upload{.content = content(2'000'000, 36)});
    ghost->uploadRate = 100'000; // 20 s
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "unwanted.flac", 2'000'000));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));

    ASSERT_TRUE(service->discardPreview(started->id));
    EXPECT_EQ(started->source->snapshot().state, selecta::audio::SourceState::Failed);
    EXPECT_FALSE(service->preview(started->id).has_value()); // forgotten
    EXPECT_TRUE(previewFiles().isEmpty());
    EXPECT_EQ(previewCache->totalBytes(), 0);
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0); // the peer's slot is free again (PREV-6)
    EXPECT_EQ(governor.peerQueued(u"vinyl_ghost"_s), 0);
    EXPECT_FALSE(service->discardPreview(started->id)); // once is enough
}

TEST_F(TransferServiceTest, DiscardingAPreviewNearItsEndStillCancelsIt)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(1'000'000, 37);
    ghost->files.insert("almost.flac", FakePeer::Upload{.content = bytes});
    ghost->uploadRate = 300'000;
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "almost.flac", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() >= 860'000; }, 10s));
    ghost->uploadRate = 1; // the last bytes crawl: it stays near its end
    ASSERT_TRUE(waitFor([&] { return service->preview(started->id)->received >= 860'000; }));
    // Past kPreviewFinishPercent, stopping lets it finish for a Get…
    service->stopPreview(started->id);
    EXPECT_EQ(service->preview(started->id)->state, selecta::core::Preview::State::Transferring);
    // …but discarding it doesn't.
    ASSERT_TRUE(service->discardPreview(started->id));
    EXPECT_FALSE(service->preview(started->id).has_value());
    EXPECT_EQ(started->source->snapshot().state, selecta::audio::SourceState::Failed);
    EXPECT_TRUE(previewFiles().isEmpty());
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, DiscardingACompletePreviewDeletesItFromTheCache)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(60'000, 38);
    ghost->files.insert("heard.mp3", FakePeer::Upload{.content = bytes});
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "heard.mp3", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->snapshot().isComplete(); }));
    ASSERT_EQ(previewCache->totalBytes(), bytes.size());

    ASSERT_TRUE(service->discardPreview(started->id));
    EXPECT_EQ(previewCache->totalBytes(), 0);
    EXPECT_TRUE(previewFiles().isEmpty());

    restart(); // gone for good: hearing it again asks the peer again
    const std::optional<TransferService::PreviewStart> again =
        service->startPreview(spec(*ghost, "heard.mp3", bytes.size()));
    ASSERT_TRUE(again.has_value());
    ASSERT_TRUE(waitFor([&] { return again->source->snapshot().isComplete(); }));
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 2);
}

TEST_F(TransferServiceTest, DiscardingAKeptPreviewLetsItsDownloadFinish)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    const QByteArray bytes = content(400'000, 39);
    ghost->files.insert("keep.flac", FakePeer::Upload{.content = bytes});
    ghost->uploadRate = 200'000; // 2 s
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "keep.flac", bytes.size()));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));
    const QSignalSpy completed(service.get(), &TransferService::completed);
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "keep.flac", bytes.size())); // K
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(service->discardPreview(started->id));
    ASSERT_TRUE(waitFor([&] { return completed.size() == 1; }, 10s));
    EXPECT_EQ(completed.at(0).at(0).value<Download>().id, *id);
    EXPECT_EQ(readFile(completed.at(0).at(0).value<Download>().localPath), bytes); // the download has its copy…
    EXPECT_EQ(ghost->receivedOf<selecta::slsk::peer::QueueUpload>().size(), 1);
    EXPECT_TRUE(waitFor([&] { return previewFiles().isEmpty(); })); // …and the cache keeps nothing
    EXPECT_EQ(previewCache->totalBytes(), 0);
}

TEST_F(TransferServiceTest, ADiscardedKeptPreviewWhoseDownloadIsCancelledStops)
{
    auto ghost = peer(u"vinyl_ghost"_s);
    ghost->files.insert("neither.flac", FakePeer::Upload{.content = content(2'000'000, 40)});
    ghost->uploadRate = 100'000; // 20 s
    const std::optional<TransferService::PreviewStart> started =
        service->startPreview(spec(*ghost, "neither.flac", 2'000'000));
    ASSERT_TRUE(started.has_value());
    ASSERT_TRUE(waitFor([&] { return started->source->available() > 0; }));
    const std::optional<qint64> id = service->enqueue(spec(*ghost, "neither.flac", 2'000'000));
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(service->discardPreview(started->id));
    EXPECT_TRUE(service->preview(started->id).has_value()); // still arriving, for the download

    service->cancel(*id); // nobody wants it now
    EXPECT_FALSE(service->preview(started->id).has_value());
    EXPECT_EQ(started->source->snapshot().state, selecta::audio::SourceState::Failed);
    EXPECT_TRUE(previewFiles().isEmpty());
    EXPECT_EQ(governor.peerActive(u"vinyl_ghost"_s), 0);
}

TEST_F(TransferServiceTest, WithoutACachePreviewsHaveNowhereToGo)
{
    stopService();
    connectNetwork();
    TransferService::Options options;
    options.incomingDir = dir.filePath(u"incoming"_s);
    service = std::make_unique<TransferService>(db, governor, *network, log, options, [this] { return now; });
    const auto ghost = peer(u"vinyl_ghost"_s);
    QString why;
    EXPECT_FALSE(service->startPreview(spec(*ghost, "nowhere.flac", 1000), &why).has_value());
    EXPECT_TRUE(why.contains(u"nowhere"_s)) << why.toStdString();
    EXPECT_EQ(governor.peerQueued(u"vinyl_ghost"_s), 0); // no permit spent on it
}
