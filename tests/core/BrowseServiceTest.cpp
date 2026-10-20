// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/BrowseService.h"

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

#include <QSignalSpy>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::Browse;
using selecta::core::BrowseService;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

selecta::slsk::FileEntry file(const QByteArray &name, quint64 size = 9'000'000)
{
    selecta::slsk::FileEntry entry;
    entry.name = selecta::slsk::RemotePath{name};
    entry.size = size;
    entry.attributes.bitrate = 320;
    entry.attributes.duration = 372;
    return entry;
}

class BrowseServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(server.start());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        network = std::make_unique<selecta::core::NetworkService>(governor, log, tokens, [this] { return now; });
        selecta::core::NetworkConfig config;
        config.host = server.address().toString();
        config.port = server.port();
        config.username = u"selecta_test"_s;
        config.listenPort = 0;
        network->start(config);
        network->setPassword(u"pw"_s);
        ASSERT_TRUE(waitFor([this] { return network->isOnline(); }));
        browse = std::make_unique<BrowseService>(db, governor, *network, [this] { return now; });
        QObject::connect(&network->peers(), &selecta::slsk::PeerNetwork::sharedFileListReceived, browse.get(),
                         &BrowseService::listReceived);
        QObject::connect(&network->peers(), &selecta::slsk::PeerNetwork::requestFailed, browse.get(),
                         [this](const QString &peer, selecta::slsk::PeerRequest request, quint32 /*token*/,
                                selecta::slsk::PeerError error) {
                             if (request == selecta::slsk::PeerRequest::SharedFileList) {
                                 browse->requestFailed(peer, selecta::core::describePeerError(peer, error));
                             }
                         });
        browse->setDownloads(
            [this](const selecta::core::DownloadSpec &spec) {
                enqueued.append(spec);
                return true;
            },
            [this](const QString &peer, const QByteArray &folder, const QList<selecta::core::SearchHit> &files,
                   const std::optional<selecta::meta::Release> &release) {
                Q_UNUSED(release)
                albums.append({peer, folder, static_cast<int>(files.size())});
                return true;
            });
    }

    void TearDown() override
    {
        browse.reset();
        network.reset();
    }

    std::unique_ptr<FakePeer> ghost()
    {
        auto peer = std::make_unique<FakePeer>(u"vinyl_ghost"_s, &server);
        EXPECT_TRUE(peer->start());
        peer->answerIndirect = false;
        selecta::slsk::SharedDirectory album;
        album.path = selecta::slsk::RemotePath{"Music\\Okuma Drift - Night Signals (2024)"};
        album.files = {file("01 - Okuma Drift - Neon Rain.mp3"), file("02 - Okuma Drift - Low Tide.mp3"),
                       file("cover.jpg", 100'000)};
        selecta::slsk::SharedDirectory singles;
        singles.path = selecta::slsk::RemotePath{"Music\\Singles"};
        singles.files = {file("Kassia Moreau - Night Transit.flac", 30'000'000)};
        peer->shares.directories = {album, singles};
        return peer;
    }

    std::optional<Browse> settled(const QString &peer)
    {
        EXPECT_TRUE(waitFor(
            [&] {
                const std::optional<Browse> state = browse->browseOf(peer);
                return state && (state->state == Browse::State::Ready || state->state == Browse::State::Failed);
            },
            10s));
        browse->waitForIdle();
        return browse->browseOf(peer);
    }

    FakeServer server;
    qint64 now = kNow;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    std::unique_ptr<selecta::core::NetworkService> network;
    std::unique_ptr<BrowseService> browse;
    QList<selecta::core::DownloadSpec> enqueued;
    struct Album
    {
        QString peer;
        QByteArray folder;
        int files = 0;
    };
    QList<Album> albums;
};

} // namespace

TEST_F(BrowseServiceTest, ListsAPeersSharesAndKeepsThemADay)
{
    auto peer = ghost();
    const QSignalSpy reset(browse.get(), &BrowseService::resultsReset);
    browse->browse(peer->username());
    const std::optional<Browse> state = settled(peer->username());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->state, Browse::State::Ready) << state->message.toStdString();
    EXPECT_EQ(state->files, 4);
    EXPECT_EQ(state->folders, 2);
    EXPECT_FALSE(state->fromCache);
    ASSERT_FALSE(reset.isEmpty());
    const QList<selecta::core::SearchResult> results = browse->results(peer->username());
    ASSERT_EQ(results.size(), 4);
    EXPECT_EQ(results.at(0).hit.path,
              QByteArray("Music\\Okuma Drift - Night Signals (2024)\\01 - Okuma Drift - Neon Rain.mp3"));
    EXPECT_TRUE(results.at(0).isAudio());
    EXPECT_FALSE(results.at(2).isAudio()); // the cover

    // Again: from the kept list, without asking the peer (BRWS-2).
    browse->close(peer->username());
    browse->browse(peer->username());
    const std::optional<Browse> again = settled(peer->username());
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(again->fromCache);
    EXPECT_EQ(again->files, 4);
    EXPECT_EQ(peer->receivedOf<selecta::slsk::peer::GetShareFileList>().size(), 1);
    EXPECT_EQ(browse->cachedPeers(), 1);

    // A day later it's asked again.
    now += BrowseService::kCacheLifetimeMs + 1;
    clock.advance(25h);
    browse->close(peer->username());
    browse->browse(peer->username());
    ASSERT_TRUE(waitFor([&] { return peer->receivedOf<selecta::slsk::peer::GetShareFileList>().size() == 2; }, 10s));
    browse->clearCache();
    EXPECT_EQ(browse->cachedPeers(), 0);
}

TEST_F(BrowseServiceTest, FilesAndFoldersDownloadFromIt)
{
    auto peer = ghost();
    browse->browse(peer->username());
    ASSERT_EQ(settled(peer->username())->state, Browse::State::Ready);
    EXPECT_EQ(browse->download(peer->username(), {0, 2, 3}), 2); // the cover isn't audio
    ASSERT_EQ(enqueued.size(), 2);
    EXPECT_EQ(enqueued.at(0).peer, u"vinyl_ghost"_s);
    EXPECT_TRUE(enqueued.at(0).file.has_value());
    EXPECT_TRUE(browse->downloadFolder(peer->username(), 1));
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(albums.constFirst().folder, QByteArray("Music\\Okuma Drift - Night Signals (2024)"));
    EXPECT_EQ(albums.constFirst().files, 3); // the whole folder, the cover too
}

TEST_F(BrowseServiceTest, APeerWhoCantBeReachedSaysSo)
{
    browse->browse(u"nobody_home"_s);
    const std::optional<Browse> state = settled(u"nobody_home"_s);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_FALSE(state->message.isEmpty());
}
