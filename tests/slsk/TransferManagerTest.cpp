// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/TransferManager.h"

#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestPermits.h"
#include "fixtures/TestUtil.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTcpSocket>

#include <algorithm>
#include <array>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace selecta::slsk;
using namespace std::chrono_literals;
using selecta::governor::Operation;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::permit;
using selecta::slsk::testing::settle;
using selecta::slsk::testing::TestPermits;
using selecta::slsk::testing::waitFor;

namespace {

constexpr qsizetype kMiB = qsizetype{1024} * 1024;

QByteArray randomContent(qsizetype size, quint32 seed)
{
    QRandomGenerator random(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (char &c : bytes) {
        c = static_cast<char>(random.bounded(256));
    }
    return bytes;
}

// Downloads land in memory: one growing byte array per remote path, kept across attempts (resume).
class MemoryStorage : public DownloadStorage
{
public:
    QIODevice *openDownload(const DownloadInfo &download) override
    {
        ++opened;
        if (refuse) {
            return nullptr;
        }
        auto &buffer = m_open[download.id];
        buffer = std::make_unique<QBuffer>(&contents[download.request.path.raw]);
        if (!buffer->open(QIODevice::WriteOnly | QIODevice::Append)) {
            return nullptr;
        }
        return buffer.get();
    }

    void closeDownload(const DownloadInfo &download, QIODevice *device) override
    {
        ++closed;
        device->close();
        m_open.erase(download.id);
    }

    bool refuse = false;
    int opened = 0;
    int closed = 0;
    QHash<QByteArray, QByteArray> contents;

private:
    std::map<TransferId, std::unique_ptr<QBuffer>> m_open;
};

class TransferManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        session.setTimeouts({2s, 2s, 5min});
        peers.setTimeouts({1s, 1s, 5s, 1s, 2s});
        transfers.setTimeouts({1500ms, 1500ms, 2s, 3s, 1s, 50ms});
        transfers.setDownloadStorage(&storage);
        ASSERT_TRUE(peers.listen(0, QHostAddress(QHostAddress::LocalHost)));
        session.setListenPort(peers.listenPort());
        ASSERT_TRUE(session.connectToServer(server.address(), server.port(), permit(Operation::Login)));
        ASSERT_TRUE(session.login(u"selecta"_s, u"pw"_s));
        ASSERT_TRUE(waitFor([&] { return session.isLoggedIn(); }));
        ASSERT_TRUE(waitFor([&] {
            const FakeServer::Client *me = server.client(u"selecta"_s);
            return me != nullptr && me->listenPort == peers.listenPort();
        }));
    }

    std::unique_ptr<FakePeer> makePeer(const QString &name, bool reachable = true)
    {
        auto peer = std::make_unique<FakePeer>(name, &server);
        EXPECT_TRUE(peer->start(reachable));
        peer->answerIndirect = !reachable; // a reachable peer is reached directly only: one connection
        return peer;
    }

    // The last state reported for download `id`.
    static std::optional<DownloadInfo> lastDownload(const QSignalSpy &spy, TransferId id)
    {
        for (qsizetype i = spy.size() - 1; i >= 0; --i) {
            auto info = spy.at(i).at(0).value<DownloadInfo>();
            if (info.id == id) {
                return info;
            }
        }
        return std::nullopt;
    }

    static std::optional<UploadInfo> lastUpload(const QSignalSpy &spy, TransferId id)
    {
        for (qsizetype i = spy.size() - 1; i >= 0; --i) {
            auto info = spy.at(i).at(0).value<UploadInfo>();
            if (info.id == id) {
                return info;
            }
        }
        return std::nullopt;
    }

    static bool reached(const QSignalSpy &spy, TransferId id, DownloadState state)
    {
        const std::optional<DownloadInfo> info = lastDownload(spy, id);
        return info && info->state == state;
    }

    TransferId download(const QString &user, const RemotePath &path, quint64 expectedSize = 0)
    {
        return transfers.download(DownloadRequest{user, path, expectedSize, false}, permit(Operation::QueueDownload));
    }

    FakeServer server;
    ServerSession session;
    TestPermits permits;
    MemoryStorage storage; // outlives the manager, which closes open downloads through it
    PeerNetwork peers{session, permits};
    std::unique_ptr<TransferManager> manager = std::make_unique<TransferManager>(peers, session);
    TransferManager &transfers = *manager;
};

} // namespace

// --- downloads -------------------------------------------------------------------------------------------------------

TEST_F(TransferManagerTest, DownloadsAFile)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"Music\\Album\\01 Track.flac"_s);
    const QByteArray content = randomContent(300'000, 1);
    alice->files.insert(path.raw, FakePeer::Upload{content, -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const QSignalSpy progress(&transfers, &TransferManager::downloadProgress);
    const TransferId id = download(u"alice"_s, path, static_cast<quint64>(content.size()));
    ASSERT_NE(id, 0U);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    EXPECT_EQ(storage.contents.value(path.raw), content);
    EXPECT_EQ(storage.opened, 1);
    EXPECT_EQ(storage.closed, 1);
    // The states in lifecycle order, none skipped but Transferring (a file can arrive in one read).
    QList<DownloadState> states;
    for (const auto &change : changes) {
        states.append(change.at(0).value<DownloadInfo>().state);
    }
    EXPECT_EQ(std::ranges::adjacent_find(states, std::greater_equal<>()), states.end()); // strictly increasing
    EXPECT_EQ(states.first(), DownloadState::Queued);
    EXPECT_TRUE(states.contains(DownloadState::Requested));
    EXPECT_TRUE(states.contains(DownloadState::Connecting));
    EXPECT_EQ(states.last(), DownloadState::Completed);
    const DownloadInfo done = *lastDownload(changes, id);
    EXPECT_EQ(done.size, static_cast<quint64>(content.size()));
    EXPECT_EQ(done.received, static_cast<quint64>(content.size()));
    EXPECT_EQ(done.startOffset, 0U);
    EXPECT_FALSE(progress.isEmpty());
    EXPECT_EQ(alice->receivedOf<peer::QueueUpload>().size(), 1);
    EXPECT_FALSE(transfers.downloadInfo(id).has_value()); // terminal: gone from the live set
}

TEST_F(TransferManagerTest, QueueUploadNeedsItsPermit)
{
    EXPECT_EQ(transfers.download(DownloadRequest{u"alice"_s, RemotePath::fromText(u"x"_s), 0, false},
                                 permit(Operation::Browse)),
              0U);
    EXPECT_EQ(transfers.download(DownloadRequest{QString(), RemotePath::fromText(u"x"_s), 0, false},
                                 permit(Operation::QueueDownload)),
              0U);
    EXPECT_EQ(transfers.download(DownloadRequest{u"alice"_s, RemotePath{}, 0, false}, permit(Operation::QueueDownload)),
              0U);
    settle();
    EXPECT_TRUE(permits.requests.isEmpty());
}

TEST_F(TransferManagerTest, SameFileTwiceIsOneDownload)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const RemotePath path = RemotePath::fromText(u"a.mp3"_s);
    const TransferId first = download(u"alice"_s, path);
    EXPECT_EQ(download(u"alice"_s, path), first);
    EXPECT_EQ(transfers.downloads().size(), 1);
}

TEST_F(TransferManagerTest, ResumesAfterAKilledTransfer)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"Music\\long.flac"_s);
    const QByteArray content = randomContent(400'000, 2);
    alice->files.insert(path.raw, FakePeer::Upload{content, 150'000, false, {}}); // the first attempt dies at 150 000
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId first = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, first, DownloadState::Failed); }));
    EXPECT_EQ(lastDownload(changes, first)->error, TransferError::ConnectionLost);
    EXPECT_EQ(storage.contents.value(path.raw).size(), 150'000);

    const TransferId second = download(u"alice"_s, path);
    ASSERT_NE(second, first);
    ASSERT_TRUE(waitFor([&] { return reached(changes, second, DownloadState::Completed); }));
    EXPECT_EQ(lastDownload(changes, second)->startOffset, 150'000U); // resumed, not restarted
    EXPECT_EQ(storage.contents.value(path.raw), content);
}

TEST_F(TransferManagerTest, ZeroSizeFromTheUploaderFallsBackToTheSearchResult)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"big.wav"_s);
    const QByteArray content = randomContent(50'000, 3);
    alice->files.insert(path.raw, FakePeer::Upload{content, -1, true, {}}); // SoulseekQt-style size 0
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path, static_cast<quint64>(content.size()));
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    EXPECT_EQ(storage.contents.value(path.raw), content);
}

TEST_F(TransferManagerTest, TwoFilesFromOnePeerAreMatchedByToken)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath a = RemotePath::fromText(u"a.flac"_s);
    const RemotePath b = RemotePath::fromText(u"b.flac"_s);
    alice->files.insert(a.raw, FakePeer::Upload{randomContent(120'000, 4), -1, false, {}});
    alice->files.insert(b.raw, FakePeer::Upload{randomContent(90'000, 5), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId first = download(u"alice"_s, a);
    const TransferId second = download(u"alice"_s, b);
    ASSERT_TRUE(waitFor([&] {
        return reached(changes, first, DownloadState::Completed) && reached(changes, second, DownloadState::Completed);
    }));
    EXPECT_EQ(storage.contents.value(a.raw), alice->files.value(a.raw).content);
    EXPECT_EQ(storage.contents.value(b.raw), alice->files.value(b.raw).content);
}

TEST_F(TransferManagerTest, CancelMidTransferIgnoresTheEcho)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"Music\\huge.flac"_s);
    alice->files.insert(path.raw, FakePeer::Upload{randomContent(32 * kMiB, 6), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const QSignalSpy progress(&transfers, &TransferManager::downloadProgress);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return !progress.isEmpty(); }));
    transfers.cancelDownload(id);
    ASSERT_TRUE(reached(changes, id, DownloadState::Cancelled));
    const qsizetype changesAtCancel = changes.size();
    EXPECT_EQ(storage.closed, 1);
    // The uploader notices the closed connection and sends UploadFailed: nothing happens on our side.
    ASSERT_TRUE(waitFor([&] { return !alice->uploadFailuresSent.isEmpty(); }));
    settle(200ms);
    EXPECT_EQ(changes.size(), changesAtCancel);
    EXPECT_LT(storage.contents.value(path.raw).size(), 32 * kMiB);
}

TEST_F(TransferManagerTest, DenialsKeepTheTextAndAreClassified)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Deny;
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    struct Case
    {
        const char16_t *reason;
        RejectionClass expected;
    };
    const std::array cases{Case{u"Too many files", RejectionClass::Retryable}, Case{u"Banned", RejectionClass::Banned},
                           Case{u"File not shared.", RejectionClass::Never},
                           Case{u"Pending shutdown.", RejectionClass::Retryable}};
    int n = 0;
    for (const auto &c : cases) {
        alice->denyReason = QString::fromUtf16(c.reason);
        const TransferId id = download(u"alice"_s, RemotePath::fromText(u"file %1.mp3"_s.arg(n++)));
        ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Failed); }));
        const DownloadInfo info = *lastDownload(changes, id);
        EXPECT_EQ(info.error, TransferError::Denied);
        ASSERT_TRUE(info.rejection.has_value());
        EXPECT_EQ(info.rejection->text, QString::fromUtf16(c.reason)); // exact text kept
        EXPECT_EQ(info.rejection->rejectionClass, c.expected);
    }
}

TEST_F(TransferManagerTest, WithdrawnQueuedDownloadIsAnsweredCancelled)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const RemotePath path = RemotePath::fromText(u"held.mp3"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(100, 'x'), -1, false, {}});
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::QueueUpload>().size() == 1; }));
    transfers.cancelDownload(id);
    // Much later the uploader's turn comes: the withdrawn download is declined so it moves on at once.
    ASSERT_TRUE(alice->startUpload(u"selecta"_s, path));
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::TransferResponse>().size() == 1; }));
    const peer::TransferResponse answer = alice->receivedOf<peer::TransferResponse>().at(0);
    EXPECT_FALSE(answer.allowed);
    EXPECT_EQ(answer.reason, u"Cancelled"_s);
    EXPECT_EQ(storage.opened, 0);
}

TEST_F(TransferManagerTest, UnsolicitedTransferRequestIsAnsweredCancelled)
{
    const auto alice = makePeer(u"alice"_s);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    alice->send(c,
                frame(peer::TransferRequest{TransferDirection::Upload, 99, RemotePath::fromText(u"gift.exe"_s), 10}));
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::TransferResponse>().size() == 1; }));
    EXPECT_FALSE(alice->receivedOf<peer::TransferResponse>().at(0).allowed);
    EXPECT_EQ(alice->receivedOf<peer::TransferResponse>().at(0).reason, u"Cancelled"_s);
}

TEST_F(TransferManagerTest, RefusedStorageDeclinesAndFails)
{
    storage.refuse = true;
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"a.mp3"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(1000, 'y'), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Failed); }));
    EXPECT_EQ(lastDownload(changes, id)->error, TransferError::LocalError);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::TransferResponse>().size() == 1; }));
    EXPECT_EQ(alice->receivedOf<peer::TransferResponse>().at(0).reason, u"Cancelled"_s);
}

TEST_F(TransferManagerTest, UploaderThatNeverConnectsTimesOut)
{
    const auto alice = makePeer(u"alice"_s);
    alice->neverConnectFile = true;
    const RemotePath path = RemotePath::fromText(u"a.mp3"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(1000, 'z'), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Failed); }));
    EXPECT_EQ(lastDownload(changes, id)->error, TransferError::Timeout);
    EXPECT_EQ(storage.closed, storage.opened);
}

TEST_F(TransferManagerTest, PlaceInQueue)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    alice->placeInQueue = 7;
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, RemotePath::fromText(u"q.mp3"_s));
    EXPECT_FALSE(transfers.requestPlaceInQueue(id, permit(Operation::Browse)));
    EXPECT_FALSE(transfers.requestPlaceInQueue(id + 100, permit(Operation::PlaceInQueue)));
    ASSERT_TRUE(transfers.requestPlaceInQueue(id, permit(Operation::PlaceInQueue)));
    ASSERT_TRUE(waitFor([&] {
        const std::optional<DownloadInfo> info = lastDownload(changes, id);
        return info && info->placeInQueue == 7U;
    }));
}

TEST_F(TransferManagerTest, UnreachableUploaderFailsTheQueueRequest)
{
    const auto ghost = makePeer(u"ghost"_s, false);
    ghost->answerIndirect = false;
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"ghost"_s, RemotePath::fromText(u"x.mp3"_s));
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Failed); }));
    EXPECT_EQ(lastDownload(changes, id)->error, TransferError::ConnectionFailed);
    EXPECT_EQ(lastDownload(changes, id)->peerError, PeerError::Unreachable);
}

TEST_F(TransferManagerTest, DownloadWhenTheUploaderMustDialBack)
{
    // We advertise no port: the uploader asks for an indirect F connection and we dial back.
    session.setListenPort(0);
    ASSERT_TRUE(waitFor([&] { return server.client(u"selecta"_s)->listenPort == 0; }));
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"Music\\indirect.flac"_s);
    const QByteArray content = randomContent(80'000, 7);
    alice->files.insert(path.raw, FakePeer::Upload{content, -1, false, {}});
    // Our own P connection to alice goes out directly; only her F connection needs the dial-back.
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    EXPECT_EQ(storage.contents.value(path.raw), content);
    EXPECT_FALSE(alice->pierceTokens.isEmpty());
}

TEST_F(TransferManagerTest, PreviewFlagTravelsWithTheDownload)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"preview.mp3"_s);
    alice->files.insert(path.raw, FakePeer::Upload{randomContent(20'000, 8), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id =
        transfers.download(DownloadRequest{u"alice"_s, path, 0, true}, permit(Operation::QueueDownload));
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    for (const auto &change : changes) {
        EXPECT_TRUE(change.at(0).value<DownloadInfo>().request.preview);
    }
}

TEST_F(TransferManagerTest, UnknownTokenOnAFileConnectionIsRefused)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const RemotePath path = RemotePath::fromText(u"t.mp3"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(10, 'a'), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::QueueUpload>().size() == 1; }));
    ASSERT_TRUE(alice->startUpload(u"selecta"_s, path));
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Requested); }));
    // An F connection carrying a token that isn't the one in the TransferRequest.
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::File);
    alice->send(c, file::encodeTransferInit(0xBADBAD));
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(c); }));
}

// --- uploads ---------------------------------------------------------------------------------------------------------

TEST_F(TransferManagerTest, QueueRequestsBecomeSignals)
{
    const auto alice = makePeer(u"alice"_s);
    const QSignalSpy requested(&transfers, &TransferManager::uploadRequested);
    const QSignalSpy places(&transfers, &TransferManager::placeInQueueRequested);
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    const RemotePath path = RemotePath::fromText(u"Collection\\a.flac"_s);
    alice->send(c, frame(peer::QueueUpload{path}));
    alice->send(c, frame(peer::PlaceInQueueRequest{path}));
    // Legacy: a downloader-sent TransferRequest is answered "Queued" and queued like QueueUpload.
    alice->send(c, frame(peer::TransferRequest{TransferDirection::Download, 5, path, std::nullopt}));
    ASSERT_TRUE(waitFor([&] { return requested.size() == 2 && places.size() == 1; }));
    EXPECT_FALSE(requested.at(0).at(2).toBool());
    EXPECT_TRUE(requested.at(1).at(2).toBool());
    EXPECT_EQ(requested.at(1).at(1).value<RemotePath>(), path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::TransferResponse>().size() == 1; }));
    const peer::TransferResponse answer = alice->receivedOf<peer::TransferResponse>().at(0);
    EXPECT_EQ(answer.token, 5U);
    EXPECT_FALSE(answer.allowed);
    EXPECT_EQ(answer.reason, u"Queued"_s);
    // core/ answers through the manager.
    transfers.sendPlaceInQueue(u"alice"_s, path, 3);
    transfers.denyUpload(u"alice"_s, RemotePath::fromText(u"other.flac"_s), u"File not shared."_s);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::UploadDenied>().size() == 1; }));
    EXPECT_EQ(alice->receivedOf<peer::PlaceInQueueResponse>().at(0).place, 3U);
    EXPECT_EQ(alice->receivedOf<peer::UploadDenied>().at(0).reason, u"File not shared."_s);
}

TEST_F(TransferManagerTest, UploadsAFileAndReportsTheSpeed)
{
    const auto bob = makePeer(u"bob"_s);
    const QByteArray content = randomContent(500'000, 9);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const QSignalSpy progress(&transfers, &TransferManager::uploadProgress);
    const RemotePath path = RemotePath::fromText(u"Collection\\song.flac"_s);
    const TransferId id =
        transfers.startUpload(UploadRequest{u"bob"_s, path, static_cast<quint64>(content.size())}, &source);
    ASSERT_NE(id, 0U);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Completed;
    }));
    ASSERT_EQ(bob->downloadedFiles.size(), 1);
    EXPECT_EQ(bob->downloadedFiles.at(0), content);
    const UploadInfo done = *lastUpload(changes, id);
    EXPECT_EQ(done.sent, static_cast<quint64>(content.size()));
    EXPECT_GT(done.averageSpeed, 0U);
    EXPECT_FALSE(progress.isEmpty());
    ASSERT_TRUE(waitFor([&] { return server.receivedOf<toserver::SendUploadSpeed>(u"selecta"_s).size() == 1; }));
    EXPECT_EQ(server.receivedOf<toserver::SendUploadSpeed>(u"selecta"_s).at(0).bytesPerSecond, done.averageSpeed);
    // TransferRequest carried our size and a non-zero token.
    const peer::TransferRequest request = bob->receivedOf<peer::TransferRequest>().at(0);
    EXPECT_EQ(request.size, static_cast<quint64>(content.size()));
    EXPECT_NE(request.token, 0U);
}

TEST_F(TransferManagerTest, UploadResumesFromTheDownloadersOffset)
{
    const auto bob = makePeer(u"bob"_s);
    bob->resumeOffset = 123'456;
    const QByteArray content = randomContent(300'000, 10);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id = transfers.startUpload(
        UploadRequest{u"bob"_s, RemotePath::fromText(u"r.flac"_s), static_cast<quint64>(content.size())}, &source);
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }));
    EXPECT_EQ(bob->downloadedFiles.at(0), content.mid(123'456));
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Completed;
    }));
    EXPECT_EQ(lastUpload(changes, id)->startOffset, 123'456U);
}

TEST_F(TransferManagerTest, OffsetPastTheEndFailsTheUpload)
{
    const auto bob = makePeer(u"bob"_s);
    bob->resumeOffset = 1'000'000; // more than the file has
    QBuffer source;
    source.setData(QByteArray(1000, 'q'));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id =
        transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"o.flac"_s), 1000}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->isTerminal();
    }));
    const UploadInfo info = *lastUpload(changes, id);
    // The downloader may also hang up first; either way the upload did not complete.
    EXPECT_EQ(info.state, UploadState::Failed);
}

TEST_F(TransferManagerTest, DeclinedUploadKeepsTheReason)
{
    const auto bob = makePeer(u"bob"_s);
    bob->declineUploads = u"Complete"_s;
    QBuffer source;
    source.setData(QByteArray(10, 'c'));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId done =
        transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"c.flac"_s), 10}, &source);
    ASSERT_TRUE(waitFor([&] { return lastUpload(changes, done) && lastUpload(changes, done)->isTerminal(); }));
    EXPECT_EQ(lastUpload(changes, done)->state, UploadState::Completed); // they already have it
    bob->declineUploads = u"Cancelled"_s;
    const TransferId declined =
        transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"d.flac"_s), 10}, &source);
    ASSERT_TRUE(waitFor([&] { return lastUpload(changes, declined) && lastUpload(changes, declined)->isTerminal(); }));
    const UploadInfo info = *lastUpload(changes, declined);
    EXPECT_EQ(info.state, UploadState::Declined);
    ASSERT_TRUE(info.rejection.has_value());
    EXPECT_EQ(info.rejection->reason, RejectionReason::Cancelled);
    settle();
    EXPECT_TRUE(server.receivedOf<toserver::SendUploadSpeed>(u"selecta"_s).isEmpty()); // nothing was sent
}

TEST_F(TransferManagerTest, UploadOverTheIndirectPath)
{
    const auto carol = makePeer(u"carol"_s, false); // not reachable directly: she dials back
    const QByteArray content = randomContent(64'000, 11);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id = transfers.startUpload(
        UploadRequest{u"carol"_s, RemotePath::fromText(u"i.flac"_s), static_cast<quint64>(content.size())}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Completed;
    }));
    EXPECT_EQ(carol->downloadedFiles.value(0), content);
}

TEST_F(TransferManagerTest, CancelUploadTellsTheDownloader)
{
    const auto bob = makePeer(u"bob"_s);
    QBuffer source;
    source.setData(randomContent(16 * kMiB, 12));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    transfers.setUploadSpeedLimit(kMiB / 4); // slow enough to cancel mid-way
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const RemotePath path = RemotePath::fromText(u"big.flac"_s);
    const TransferId id = transfers.startUpload(UploadRequest{u"bob"_s, path, 16 * kMiB}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Transferring;
    }));
    transfers.cancelUpload(id, u"Banned"_s);
    EXPECT_EQ(lastUpload(changes, id)->state, UploadState::Cancelled);
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::UploadDenied>().size() == 1; }));
    EXPECT_EQ(bob->receivedOf<peer::UploadDenied>().at(0).reason, u"Banned"_s);
    EXPECT_EQ(bob->receivedOf<peer::UploadDenied>().at(0).path, path);
}

TEST_F(TransferManagerTest, SpeedLimitPacesUploads)
{
    const auto bob = makePeer(u"bob"_s);
    const QByteArray content = randomContent(600'000, 13);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    transfers.setUploadSpeedLimit(300'000); // 2 s for the file, give or take a tick
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    QElapsedTimer clock;
    clock.start();
    const TransferId id = transfers.startUpload(
        UploadRequest{u"bob"_s, RemotePath::fromText(u"p.flac"_s), static_cast<quint64>(content.size())}, &source);
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }, 10s));
    EXPECT_GE(clock.elapsed(), 1500);
    EXPECT_EQ(bob->downloadedFiles.at(0), content);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Completed;
    }));
}

TEST_F(TransferManagerTest, InvalidUploadsAreRefused)
{
    QBuffer closed;
    EXPECT_EQ(transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"x"_s), 1}, &closed), 0U);
    EXPECT_EQ(transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"x"_s), 1}, nullptr), 0U);
    QBuffer open;
    ASSERT_TRUE(open.open(QIODevice::ReadOnly));
    EXPECT_EQ(transfers.startUpload(UploadRequest{QString(), RemotePath::fromText(u"x"_s), 1}, &open), 0U);
}

// --- edge cases ------------------------------------------------------------------------------------------------------

TEST_F(TransferManagerTest, InboundFileConnectionServesAWaitingUpload)
{
    // A downloader we can't reach opens the F connection itself (Soulseek.NET's "second chance").
    const auto dave = makePeer(u"dave"_s, false);
    dave->answerIndirect = false;
    const QByteArray content = randomContent(70'000, 14);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    // Dave's own P connection carries our TransferRequest; he allows it, then connects F himself.
    dave->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::Peer);
    ASSERT_TRUE(waitFor([&] { return peers.connectionCount(ConnectionType::Peer) == 1; }));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id = transfers.startUpload(
        UploadRequest{u"dave"_s, RemotePath::fromText(u"s.flac"_s), static_cast<quint64>(content.size())}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->state == UploadState::Connecting;
    }));
    dave->connectAsDownloader(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort());
    ASSERT_TRUE(waitFor([&] { return dave->downloadedFiles.size() == 1; }));
    EXPECT_EQ(dave->downloadedFiles.at(0), content);
}

TEST_F(TransferManagerTest, DownloaderHangingUpFailsTheUploadAndSaysSo)
{
    const auto bob = makePeer(u"bob"_s);
    bob->closeDownloadAfter = 100'000;
    QBuffer source;
    source.setData(randomContent(2 * kMiB, 15));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const RemotePath path = RemotePath::fromText(u"h.flac"_s);
    const TransferId id = transfers.startUpload(UploadRequest{u"bob"_s, path, 2 * kMiB}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->isTerminal();
    }));
    EXPECT_EQ(lastUpload(changes, id)->state, UploadState::Failed);
    EXPECT_EQ(lastUpload(changes, id)->error, TransferError::ConnectionLost);
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::UploadFailed>().size() == 1; }));
    EXPECT_EQ(bob->receivedOf<peer::UploadFailed>().at(0).path, path);
    EXPECT_TRUE(server.receivedOf<toserver::SendUploadSpeed>(u"selecta"_s).isEmpty());
}

TEST_F(TransferManagerTest, SilentDownloaderTimesTheUploadOut)
{
    const auto bob = makePeer(u"bob"_s);
    bob->ignoreTransferRequests = true;
    QBuffer source;
    source.setData(QByteArray(100, 'n'));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id =
        transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"n.flac"_s), 100}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->isTerminal();
    }));
    EXPECT_EQ(lastUpload(changes, id)->error, TransferError::Timeout);
    settle();
    EXPECT_TRUE(bob->receivedOf<peer::UploadFailed>().isEmpty()); // never allowed: nothing to re-queue
}

TEST_F(TransferManagerTest, StalledUploaderTimesTheDownloadOut)
{
    const auto alice = makePeer(u"alice"_s);
    alice->stallUploads = true;
    const RemotePath path = RemotePath::fromText(u"stall.flac"_s);
    alice->files.insert(path.raw, FakePeer::Upload{randomContent(10'000, 16), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Failed); }));
    EXPECT_EQ(lastDownload(changes, id)->error, TransferError::Timeout);
}

TEST_F(TransferManagerTest, EmptyFileCompletesAtOnce)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath path = RemotePath::fromText(u"empty.cue"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    EXPECT_TRUE(storage.contents.value(path.raw).isEmpty());
}

TEST_F(TransferManagerTest, UploadFailedForAQueuedDownloadIsIgnored)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const RemotePath path = RemotePath::fromText(u"echo.mp3"_s);
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::QueueUpload>().size() == 1; }));
    alice->sendTo(u"selecta"_s, frame(peer::UploadFailed{path}));
    settle(200ms);
    EXPECT_TRUE(reached(changes, id, DownloadState::Queued));
    EXPECT_TRUE(transfers.downloadInfo(id).has_value());
}

TEST_F(TransferManagerTest, TokenArrivingInPiecesStillMatches)
{
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    alice->neverConnectFile = true; // we play the uploader's F connection by hand
    const RemotePath path = RemotePath::fromText(u"pieces.flac"_s);
    const QByteArray content = randomContent(5'000, 17);
    alice->files.insert(path.raw, FakePeer::Upload{content, -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::QueueUpload>().size() == 1; }));
    ASSERT_TRUE(alice->startUpload(u"selecta"_s, path));
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::TransferResponse>().size() == 1; }));
    const quint32 token = alice->receivedOf<peer::TransferResponse>().at(0).token;
    QTcpSocket raw;
    raw.connectToHost(QHostAddress::LocalHost, peers.listenPort());
    ASSERT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::ConnectedState; }));
    raw.write(frame(peerinit::PeerInit{u"alice"_s, ConnectionType::File, 0}));
    const QByteArray tokenBytes = file::encodeTransferInit(token);
    for (const char byte : tokenBytes) {
        raw.write(&byte, 1);
        raw.flush();
        settle(20ms);
    }
    ASSERT_TRUE(waitFor([&] { return raw.bytesAvailable() >= file::kOffsetBytes; }));
    EXPECT_EQ(file::decodeOffset(raw.read(file::kOffsetBytes)), 0U);
    raw.write(content);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Completed); }));
    EXPECT_EQ(storage.contents.value(path.raw), content);
    EXPECT_TRUE(waitFor([&] { return raw.state() == QAbstractSocket::UnconnectedState; })); // the downloader closes
}

TEST_F(TransferManagerTest, ProgressIsCoalesced)
{
    transfers.setTimeouts({2s, 2s, 3s, 5s, 1s, 200ms});
    const auto bob = makePeer(u"bob"_s);
    const QByteArray content = randomContent(900'000, 18);
    QBuffer source;
    source.setData(content);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    transfers.setUploadSpeedLimit(600'000); // about 1.5 s of many small writes
    const QSignalSpy progress(&transfers, &TransferManager::uploadProgress);
    QElapsedTimer clock;
    clock.start();
    QList<qint64> times;
    const QObject receiver; // destroyed first: the connection never outlives `times` and `clock`
    QObject::connect(
        &transfers, &TransferManager::uploadProgress, &receiver,
        [times = &times, clock = &clock](TransferId, quint64, quint64) { times->append(clock->elapsed()); });
    transfers.startUpload(
        UploadRequest{u"bob"_s, RemotePath::fromText(u"c.flac"_s), static_cast<quint64>(content.size())}, &source);
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }, 10s));
    settle(300ms);
    ASSERT_GE(times.size(), 3);
    // Apart from the final one, signals are at least an interval apart (with some scheduling slack).
    for (qsizetype i = 1; i + 1 < times.size(); ++i) {
        EXPECT_GE(times.at(i) - times.at(i - 1), 150) << "signal " << i;
    }
    QList<quint64> done;
    for (const auto &signal : progress) {
        done.append(signal.at(1).toULongLong());
    }
    EXPECT_EQ(done.constLast(), static_cast<quint64>(content.size()));
    EXPECT_TRUE(std::ranges::is_sorted(done));
}

TEST_F(TransferManagerTest, ReceiversMayCancelDownloadsFromInsideASignal)
{
    const auto alice = makePeer(u"alice"_s);
    const RemotePath early = RemotePath::fromText(u"early.flac"_s);
    const RemotePath late = RemotePath::fromText(u"late.flac"_s);
    alice->files.insert(early.raw, FakePeer::Upload{randomContent(200'000, 19), -1, false, {}});
    alice->files.insert(late.raw, FakePeer::Upload{randomContent(8 * kMiB, 20), -1, false, {}});
    // A controller that cancels on a given state, connected before an observer that keeps a log.
    QObject::connect(&transfers, &TransferManager::downloadChanged, &transfers,
                     [this, early, late](const DownloadInfo &info) {
                         if ((info.request.path == early && info.state == DownloadState::Connecting) ||
                             (info.request.path == late && info.state == DownloadState::Transferring)) {
                             transfers.cancelDownload(info.id);
                         }
                     });
    QList<std::pair<TransferId, std::optional<DownloadState>>> log; // nullopt: a progress signal
    const QObject observer;                                         // destroyed before `log`
    QObject::connect(&transfers, &TransferManager::downloadChanged, &observer,
                     [log = &log](const DownloadInfo &info) { log->append({info.id, info.state}); });
    QObject::connect(&transfers, &TransferManager::downloadProgress, &observer,
                     [log = &log](TransferId id, quint64, quint64) { log->append({id, std::nullopt}); });
    const auto states = [&log](TransferId id) {
        QList<DownloadState> seen;
        for (const auto &[entry, state] : std::as_const(log)) {
            if (entry == id && state) {
                seen.append(*state);
            }
        }
        return seen;
    };
    const TransferId first = download(u"alice"_s, early);
    const TransferId second = download(u"alice"_s, late);
    ASSERT_TRUE(waitFor([&] {
        return states(first).value(states(first).size() - 1) == DownloadState::Cancelled &&
               states(second).value(states(second).size() - 1) == DownloadState::Cancelled;
    }));
    settle(300ms);
    // The observer saw each download's states in order, Cancelled last, and nothing about it afterwards.
    EXPECT_EQ(states(first), (QList{DownloadState::Queued, DownloadState::Requested, DownloadState::Connecting,
                                    DownloadState::Cancelled}));
    EXPECT_EQ(states(second), (QList{DownloadState::Queued, DownloadState::Requested, DownloadState::Connecting,
                                     DownloadState::Transferring, DownloadState::Cancelled}));
    for (const TransferId id : {first, second}) {
        const auto terminal = std::ranges::find(log, std::make_pair(id, std::optional(DownloadState::Cancelled)));
        ASSERT_NE(terminal, log.end());
        EXPECT_TRUE(
            std::none_of(std::next(terminal), log.end(), [id](const auto &entry) { return entry.first == id; }));
    }
    EXPECT_EQ(storage.closed, storage.opened);
    EXPECT_TRUE(transfers.downloads().isEmpty());
}

TEST_F(TransferManagerTest, ReceiversMayCancelUploadsFromInsideASignal)
{
    const auto bob = makePeer(u"bob"_s);
    QBuffer source;
    source.setData(randomContent(100'000, 21));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    QObject::connect(&transfers, &TransferManager::uploadChanged, &transfers, [this](const UploadInfo &info) {
        if (info.state == UploadState::Connecting) {
            transfers.cancelUpload(info.id);
        }
    });
    const QSignalSpy changes(&transfers, &TransferManager::uploadChanged);
    const TransferId id =
        transfers.startUpload(UploadRequest{u"bob"_s, RemotePath::fromText(u"x.flac"_s), 100'000}, &source);
    ASSERT_TRUE(waitFor([&] {
        const std::optional<UploadInfo> info = lastUpload(changes, id);
        return info && info->isTerminal();
    }));
    settle(300ms);
    QList<UploadState> states;
    for (const auto &change : changes) {
        states.append(change.at(0).value<UploadInfo>().state);
    }
    EXPECT_EQ(states, (QList{UploadState::Requested, UploadState::Connecting, UploadState::Cancelled}));
    // The F connection was given up before it was made.
    const QList<toserver::ConnectToPeer> requests = server.receivedOf<toserver::ConnectToPeer>(u"selecta"_s);
    for (const toserver::ConnectToPeer &request : requests) {
        EXPECT_NE(request.type, ConnectionType::File);
    }
    EXPECT_TRUE(bob->downloadedFiles.isEmpty());
    EXPECT_TRUE(transfers.uploads().isEmpty());
}

TEST_F(TransferManagerTest, FileConnectionThatSendsNoTokenIsDropped)
{
    transfers.setTimeouts({5s, 300ms, 2s, 3s, 1s, 50ms});
    const auto alice = makePeer(u"alice"_s);
    alice->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    alice->neverConnectFile = true; // only the connection below
    const RemotePath path = RemotePath::fromText(u"mute.flac"_s);
    alice->files.insert(path.raw, FakePeer::Upload{QByteArray(100, 'm'), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path);
    ASSERT_TRUE(waitFor([&] { return alice->receivedOf<peer::QueueUpload>().size() == 1; }));
    ASSERT_TRUE(alice->startUpload(u"selecta"_s, path));
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Requested); }));
    // An F connection that identifies itself, then says nothing.
    const quint64 c = alice->connectTo(u"selecta"_s, QHostAddress::LocalHost, peers.listenPort(), ConnectionType::File);
    EXPECT_TRUE(waitFor([&] { return !alice->isOpen(c); }, 2s));
    // The download still waits for a proper one.
    EXPECT_TRUE(reached(changes, id, DownloadState::Requested));
}

TEST_F(TransferManagerTest, DestroyingTheManagerClosesOpenDownloads)
{
    const auto alice = makePeer(u"alice"_s);
    alice->stallUploads = true;
    const RemotePath path = RemotePath::fromText(u"open.flac"_s);
    alice->files.insert(path.raw, FakePeer::Upload{randomContent(10'000, 22), -1, false, {}});
    const QSignalSpy changes(&transfers, &TransferManager::downloadChanged);
    const TransferId id = download(u"alice"_s, path, 10'000);
    ASSERT_TRUE(waitFor([&] { return reached(changes, id, DownloadState::Connecting); }));
    EXPECT_EQ(storage.opened, 1);
    EXPECT_EQ(storage.closed, 0);
    const qsizetype signalsBefore = changes.size();
    manager.reset();
    EXPECT_EQ(storage.closed, 1);
    EXPECT_EQ(changes.size(), signalsBefore); // and nothing said
}
