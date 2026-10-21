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
#include "slsk/Codec.h"
#include "slsk/Frame.h"
#include "slsk/PeerMessages.h"
#include "slsk/Protocol.h"
#include "slsk/Zlib.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QSignalSpy>
#include <QtEndian>

#include <algorithm>
#include <cstddef>
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

TEST_F(BrowseServiceTest, ANestedTreeIsListedWhole)
{
    auto peer = std::make_unique<FakePeer>(u"deep_digger"_s, &server);
    ASSERT_TRUE(peer->start());
    peer->answerIndirect = false;
    // Folders inside folders, files at every level, odd and non-audio files too: every one of them is listed.
    QList<selecta::slsk::SharedDirectory> directories;
    const QList<QByteArray> paths = {"@@music",
                                     "@@music\\Artist",
                                     "@@music\\Artist\\Album (2001)",
                                     R"(@@music\Artist\Album (2001)\CD1)",
                                     "@@music\\SPEEDTEST",
                                     "@@other\\Ünïcödé"};
    int expected = 0;
    for (const QByteArray &path : paths) {
        selecta::slsk::SharedDirectory directory;
        directory.path = selecta::slsk::RemotePath{path};
        directory.files = {file("01 - Track.flac"), file("10MB.bin", 10'000'000), file("notes.txt", 100)};
        expected += static_cast<int>(directory.files.size());
        directories.append(directory);
    }
    peer->shares.directories = directories;
    browse->browse(peer->username());
    const std::optional<Browse> state = settled(peer->username());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->state, Browse::State::Ready) << state->message.toStdString();
    EXPECT_EQ(state->files, expected);
    EXPECT_EQ(state->folders, paths.size());
    const QList<selecta::core::SearchResult> results = browse->results(peer->username());
    ASSERT_EQ(results.size(), expected);
    EXPECT_EQ(results.at((3 * 3) + 1).hit.path, QByteArray("@@music\\Artist\\Album (2001)\\CD1\\10MB.bin"));
    EXPECT_EQ(results.at((3 * 3) + 1).folder, u"@@music\\Artist\\Album (2001)\\CD1"_s);
}

TEST_F(BrowseServiceTest, APeerWhoSharesNothingSaysSo)
{
    auto peer = std::make_unique<FakePeer>(u"empty_shelf"_s, &server);
    ASSERT_TRUE(peer->start());
    peer->answerIndirect = false;
    browse->browse(peer->username());
    const std::optional<Browse> state = settled(peer->username());
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->state, Browse::State::Ready);
    EXPECT_EQ(state->files, 0);
    EXPECT_TRUE(state->message.contains(u"shares nothing"_s)) << state->message.toStdString();
    EXPECT_TRUE(browse->results(peer->username()).isEmpty());
}

TEST_F(BrowseServiceTest, AMalformedListSaysItCouldntBeRead)
{
    auto peer = std::make_unique<FakePeer>(u"garbled"_s, &server);
    ASSERT_TRUE(peer->start());
    peer->answerIndirect = false;
    peer->ignoreRequests = true;
    FakePeer *raw = peer.get();
    peer->onMessage = [raw](quint64 connection, const QString & /*from*/, const selecta::slsk::PeerMessage &message) {
        if (std::holds_alternative<selecta::slsk::peer::GetShareFileList>(message)) {
            selecta::slsk::Writer body;
            body.u32(1000); // a thousand folders announced, none follows
            raw->send(connection,
                      selecta::slsk::encodeFrame(selecta::slsk::CodeWidth::Word,
                                                 static_cast<quint32>(selecta::slsk::PeerCode::SharedFileListResponse),
                                                 selecta::slsk::zlib::deflate(body.data())));
        }
    };
    browse->browse(peer->username());
    const std::optional<Browse> state = settled(peer->username());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_EQ(state->message, u"garbled's answer couldn't be read."_s);
    EXPECT_EQ(browse->cachedPeers(), 0);
}

TEST_F(BrowseServiceTest, AListTooBigForItsFrameSaysSo)
{
    auto peer = std::make_unique<FakePeer>(u"hoarder"_s, &server);
    ASSERT_TRUE(peer->start());
    peer->answerIndirect = false;
    peer->ignoreRequests = true;
    FakePeer *raw = peer.get();
    peer->onMessage = [raw](quint64 connection, const QString & /*from*/, const selecta::slsk::PeerMessage &message) {
        if (std::holds_alternative<selecta::slsk::peer::GetShareFileList>(message)) {
            QByteArray header(8, '\0');
            qToLittleEndian<quint32>(static_cast<quint32>(selecta::slsk::protocol::kShareListFrameMax + 5),
                                     header.data());
            qToLittleEndian<quint32>(static_cast<quint32>(selecta::slsk::PeerCode::SharedFileListResponse),
                                     header.data() + 4);
            raw->send(connection, header);
        }
    };
    browse->browse(peer->username());
    const std::optional<Browse> state = settled(peer->username());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_EQ(state->message, u"hoarder's list of shared files is too big for Selecta to read."_s);
}

TEST_F(BrowseServiceTest, AListOverTheFileCapIsRefusedAndAtTheCapIsListed)
{
    // Direct from the network: the cap is checked on what arrives, whatever its frame size.
    const auto listOf = [](int files) {
        selecta::slsk::SharedFileList list;
        constexpr int kPerFolder = 1000;
        for (int first = 0; first < files; first += kPerFolder) {
            selecta::slsk::SharedDirectory directory;
            directory.path = selecta::slsk::RemotePath{"Music\\Folder " + QByteArray::number(first)};
            for (int i = first; i < std::min(files, first + kPerFolder); ++i) {
                directory.files.append(file("Track " + QByteArray::number(i) + ".mp3"));
            }
            list.directories.append(directory);
        }
        return list;
    };
    browse->browse(u"big_one"_s); // not online at the fake server: fails, but the browse is open
    settled(u"big_one"_s);
    const QSignalSpy reset(browse.get(), &BrowseService::resultsReset);
    browse->listReceived(u"big_one"_s, listOf(BrowseService::kMaxFiles + 1));
    std::optional<Browse> state = browse->browseOf(u"big_one"_s);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_EQ(state->files, BrowseService::kMaxFiles + 1);
    EXPECT_TRUE(state->message.contains(u"too big"_s)) << state->message.toStdString();
    browse->waitForIdle();
    EXPECT_TRUE(reset.isEmpty()); // nothing analysed
    EXPECT_TRUE(browse->results(u"big_one"_s).isEmpty());
    EXPECT_EQ(browse->cachedPeers(), 0); // nor kept

    browse->listReceived(u"big_one"_s, listOf(BrowseService::kMaxFiles));
    state = settled(u"big_one"_s);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Ready) << state->message.toStdString();
    EXPECT_EQ(state->files, BrowseService::kMaxFiles);
    EXPECT_EQ(browse->results(u"big_one"_s).size(), BrowseService::kMaxFiles);

    // A list too big after one that wasn't: the old one isn't shown as if it were the new one.
    browse->listReceived(u"big_one"_s, listOf(BrowseService::kMaxFiles + 1));
    EXPECT_EQ(browse->browseOf(u"big_one"_s)->state, Browse::State::Failed);
    EXPECT_TRUE(browse->results(u"big_one"_s).isEmpty());
    ASSERT_FALSE(reset.isEmpty());
    EXPECT_TRUE(reset.constLast().at(1).value<QList<selecta::core::SearchResult>>().isEmpty());
}

TEST(BrowseServiceText, EveryFailureSaysWhy)
{
    using selecta::slsk::PeerError;
    const QString peer = u"vinyl_ghost"_s;
    EXPECT_EQ(selecta::core::describePeerError(peer, PeerError::UserOffline), u"vinyl_ghost is offline."_s);
    EXPECT_TRUE(selecta::core::describePeerError(peer, PeerError::Timeout).contains(u"in time"_s));
    EXPECT_TRUE(selecta::core::describePeerError(peer, PeerError::Unreachable).contains(u"couldn't be reached"_s));
    EXPECT_TRUE(selecta::core::describePeerError(peer, PeerError::TooLarge).contains(u"too big"_s));
    for (const PeerError error :
         {PeerError::None, PeerError::NotLoggedIn, PeerError::NotPermitted, PeerError::SocketLimit,
          PeerError::ConnectionClosed, PeerError::ProtocolError, PeerError::Cancelled}) {
        EXPECT_FALSE(selecta::core::describePeerError(peer, error).isEmpty());
    }
}

TEST_F(BrowseServiceTest, AListWhosePathsComeToTooMuchIsRefused)
{
    browse->browse(u"long_names"_s); // not at the fake server: fails, but the browse is open
    settled(u"long_names"_s);
    const QSignalSpy reset(browse.get(), &BrowseService::resultsReset);

    // A few files under one huge folder name: few files, but every one carries the whole name.
    selecta::slsk::SharedFileList huge;
    selecta::slsk::SharedDirectory folder;
    folder.path = selecta::slsk::RemotePath{QByteArray(static_cast<qsizetype>(1024 * 1024), 'x')};
    for (int i = 0; i < 65; ++i) {
        folder.files.append(file("Track " + QByteArray::number(i) + ".mp3"));
    }
    huge.directories.append(folder);
    browse->listReceived(u"long_names"_s, huge);
    std::optional<Browse> state = browse->browseOf(u"long_names"_s);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_TRUE(state->message.contains(u"too big"_s)) << state->message.toStdString();
    browse->waitForIdle();
    EXPECT_TRUE(reset.isEmpty());        // nothing analysed
    EXPECT_EQ(browse->cachedPeers(), 0); // nor kept

    // The same name with fewer files is listed.
    huge.directories.first().files.resize(10);
    browse->listReceived(u"long_names"_s, huge);
    state = settled(u"long_names"_s);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, Browse::State::Ready) << state->message.toStdString();
    EXPECT_EQ(browse->results(u"long_names"_s).size(), 10);

    // Folders nested a level per letter: short paths, but more folders than the tree would show.
    selecta::slsk::SharedFileList deep;
    QByteArray path = "a";
    for (int level = 1; level < 600; ++level) {
        path += "\\a";
    }
    for (int i = 0; i < 2000; ++i) {
        selecta::slsk::SharedDirectory directory;
        directory.path = selecta::slsk::RemotePath{QByteArray::number(i) + '\\' + path};
        directory.files = {file("t.mp3")};
        deep.directories.append(directory);
    }
    browse->listReceived(u"long_names"_s, deep);
    state = browse->browseOf(u"long_names"_s);
    EXPECT_EQ(state->state, Browse::State::Failed);
    EXPECT_TRUE(state->message.contains(u"too big"_s)) << state->message.toStdString();
    EXPECT_TRUE(browse->results(u"long_names"_s).isEmpty());
}
