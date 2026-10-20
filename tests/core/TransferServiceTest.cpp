// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/TransferService.h"

#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/GetTracksService.h"
#include "core/NetworkService.h"
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
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>

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

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

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

    void makeService()
    {
        TransferService::Options options;
        options.incomingDir = dir.filePath(u"incoming"_s);
        options.previewDir = dir.filePath(u"previews"_s);
        options.retryDelaysMs = {100, 100, 100};
        options.pacedRetryMs = 100;
        service = std::make_unique<TransferService>(db, governor, *network, log, options, [this] { return now; });
    }

    // What a quit does: the service lets go of the manager, the network goes, then the service.
    void stopService()
    {
        if (service) {
            service->shutdown();
        }
        network.reset();
        service.reset();
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
