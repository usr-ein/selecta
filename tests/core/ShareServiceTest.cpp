// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ShareService.h"

#include "SyntheticAudio.h"
#include "base/Clock.h"
#include "core/ActivityLog.h"
#include "core/NetworkService.h"
#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"
#include "governor/Governor.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::NetworkService;
using selecta::core::ShareService;
using selecta::slsk::RemotePath;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;
namespace peer = selecta::slsk::peer;
namespace synth = selecta::testing;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

quint16 freePort()
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : quint16{0};
}

class ShareServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(server.start());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        session = selecta::storage::SessionLog(db).start(kNow);

        root = dir.filePath(u"Music"_s);
        ASSERT_TRUE(QDir().mkpath(root + u"/Kel Varo"_s));
        flac = synth::makeFlac(synth::quantised(synth::sine(440.0, 1.0), 16));
        writeFile(root + u"/Kel Varo/Kel Varo - Amber Engine.flac"_s, flac);
        writeFile(root + u"/Kel Varo/Kel Varo - Night Ferry.mp3"_s, synth::makeMp3(synth::sine(330.0, 1.0)));

        network = std::make_unique<NetworkService>(governor, log, tokens, [] { return kNow; });
        selecta::core::NetworkConfig config;
        config.host = server.address().toString();
        config.port = server.port();
        config.username = u"selecta_test"_s;
        config.listenPort = freePort();
        network->start(config);
        network->setPassword(u"pw"_s);
        ASSERT_TRUE(waitFor([this] { return network->isOnline(); }));
        listenPort = config.listenPort;

        service = std::make_unique<ShareService>(db, governor, *network, log, clock, session, [] { return kNow; });
        service->setRoots({root}, {});
        service->waitForIdle();
    }

    void TearDown() override
    {
        service.reset();
        network.reset();
    }

    static void writeFile(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    std::unique_ptr<FakePeer> peer(const QString &name)
    {
        auto fake = std::make_unique<FakePeer>(name, &server);
        EXPECT_TRUE(fake->start());
        return fake;
    }

    // A P connection from `from` to us, on our listening port.
    quint64 connect(FakePeer &from) const
    {
        return from.connectTo(u"selecta_test"_s, QHostAddress::LocalHost, listenPort,
                              selecta::slsk::ConnectionType::Peer);
    }

    QTemporaryDir dir;
    FakeServer server;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    int session = 0;
    QString root;
    QByteArray flac;
    quint16 listenPort = 0;
    std::unique_ptr<NetworkService> network;
    std::unique_ptr<ShareService> service;
};

} // namespace

TEST_F(ShareServiceTest, SharesTheCollectionFoldersAndSaysSo)
{
    EXPECT_EQ(service->status().files, 2);
    EXPECT_EQ(service->status().roots, QStringList{root});
    EXPECT_FALSE(service->status().building);
    // The server hears what we share.
    ASSERT_TRUE(waitFor([&] {
        const auto counts = server.receivedOf<selecta::slsk::toserver::SharedFoldersFiles>(u"selecta_test"_s);
        return !counts.isEmpty() && counts.constLast().files == 2; // clazy:exclude=missing-typeinfo
    }));
}

TEST_F(ShareServiceTest, AnswersSearchesThatReachIt)
{
    const auto bob = peer(u"bob"_s);
    ASSERT_TRUE(
        server.sendTo(u"selecta_test"_s, selecta::slsk::fromserver::SearchRequest{u"bob"_s, 77, u"amber engine"_s}));
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::FileSearchResponse>().size() == 1; }));
    const peer::FileSearchResponse answer = bob->receivedOf<peer::FileSearchResponse>().at(0);
    EXPECT_EQ(answer.token, 77U);
    ASSERT_EQ(answer.results.size(), 1);
    EXPECT_EQ(answer.results.at(0).name.display(), u"Music\\Kel Varo\\Kel Varo - Amber Engine.flac"_s);
    EXPECT_TRUE(answer.slotFree);
    EXPECT_EQ(service->status().searchesAnswered, 1);

    // Our own searches (Soulfind echoes them) and searches that find nothing get no answer.
    ASSERT_TRUE(server.sendTo(u"selecta_test"_s,
                              selecta::slsk::fromserver::SearchRequest{u"selecta_test"_s, 78, u"amber engine"_s}));
    ASSERT_TRUE(server.sendTo(u"selecta_test"_s,
                              selecta::slsk::fromserver::SearchRequest{u"bob"_s, 79, u"nothing like this"_s}));
    QTest::qWait(300);
    EXPECT_EQ(bob->receivedOf<peer::FileSearchResponse>().size(), 1);
}

TEST_F(ShareServiceTest, UploadsWhatAPeerQueuesAndRecordsIt)
{
    const auto bob = peer(u"bob"_s);
    const quint64 connection = connect(*bob);
    const RemotePath path = RemotePath::fromText(u"Music\\Kel Varo\\Kel Varo - Amber Engine.flac"_s);
    bob->send(connection, selecta::slsk::frame(peer::QueueUpload{path}));
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }, 15s));
    EXPECT_EQ(bob->downloadedFiles.at(0), flac);
    ASSERT_TRUE(waitFor([&] { return service->status().uploadedSession.files == 1; }));
    EXPECT_EQ(service->status().uploadedSession.bytes, flac.size());
    EXPECT_EQ(service->status().uploadedAllTime.files, 1);
    EXPECT_EQ(service->status().uploading, 0);
    EXPECT_TRUE(service->uploads().isEmpty());
}

TEST_F(ShareServiceTest, FilesThatArentSharedAreRefused)
{
    const auto bob = peer(u"bob"_s);
    const quint64 connection = connect(*bob);
    // Not in the index, and a path that climbs out of it: both are simply not shared.
    bob->send(connection, selecta::slsk::frame(peer::QueueUpload{RemotePath::fromText(u"Music\\nope.flac"_s)}));
    bob->send(connection,
              selecta::slsk::frame(peer::QueueUpload{RemotePath::fromText(u"Music\\..\\..\\etc\\hosts"_s)}));
    ASSERT_TRUE(waitFor([&] { return bob->receivedOf<peer::UploadDenied>().size() == 2; }));
    const QList<peer::UploadDenied> denials = bob->receivedOf<peer::UploadDenied>();
    for (const peer::UploadDenied &denied : denials) {
        EXPECT_EQ(denied.reason, u"File not shared."_s);
    }
    EXPECT_TRUE(bob->downloadedFiles.isEmpty());
}

TEST_F(ShareServiceTest, PausedItAnswersNothingAndStartsNothing)
{
    governor.setPaused(true);
    const auto bob = peer(u"bob"_s);
    ASSERT_TRUE(
        server.sendTo(u"selecta_test"_s, selecta::slsk::fromserver::SearchRequest{u"bob"_s, 80, u"amber engine"_s}));
    const quint64 connection = connect(*bob);
    const RemotePath path = RemotePath::fromText(u"Music\\Kel Varo\\Kel Varo - Amber Engine.flac"_s);
    bob->send(connection, selecta::slsk::frame(peer::QueueUpload{path}));
    QTest::qWait(500);
    EXPECT_TRUE(bob->receivedOf<peer::FileSearchResponse>().isEmpty());
    EXPECT_TRUE(bob->downloadedFiles.isEmpty());
    EXPECT_EQ(service->status().queued, 1); // it waits in the queue

    // Resumed: the queue moves.
    governor.setPaused(false);
    ASSERT_TRUE(waitFor([&] { return bob->downloadedFiles.size() == 1; }, 15s));
}

TEST_F(ShareServiceTest, PeersSeeOurUploadState)
{
    const selecta::slsk::peer::UserInfoResponse info = service->userInfo(u"bob"_s);
    EXPECT_TRUE(info.description.startsWith(u"Selecta"_s));
    EXPECT_EQ(info.totalUploads, 3U);
    EXPECT_TRUE(info.slotsFree);
    EXPECT_FALSE(service->sharedFileList(u"bob"_s).isEmpty());
    EXPECT_EQ(service->folderContents(u"bob"_s, RemotePath::fromText(u"Music\\Kel Varo"_s)).size(), 1);
}
