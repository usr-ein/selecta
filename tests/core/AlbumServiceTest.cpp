// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/AlbumService.h"

#include "SyntheticAudio.h"
#include "base/Clock.h"
#include "collection/Scanner.h"
#include "core/ActivityLog.h"
#include "core/CollectionStore.h"
#include "core/InboxService.h"
#include "core/NetworkService.h"
#include "core/TransferService.h"
#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "fixtures/TestUtil.h"
#include "governor/Governor.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::core::AlbumDownload;
using selecta::core::AlbumReport;
using selecta::core::AlbumService;
using selecta::core::Download;
using selecta::core::InboxEntry;
using selecta::core::SearchHit;
using selecta::library::InboxItem;
using selecta::slsk::testing::FakePeer;
using selecta::slsk::testing::FakeServer;
using selecta::slsk::testing::waitFor;
namespace synth = selecta::testing;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

// The album folder vinyl_ghost shares.
QByteArray albumFolder()
{
    return QByteArrayLiteral("Music\\Okuma Drift - Night Signals (2024)");
}

QByteArray jpeg(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(QColor(90, 200, 40));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

selecta::meta::Release neonRain()
{
    selecta::meta::Release release;
    release.source = selecta::meta::Source::Spotify;
    release.title = u"Neon Rain"_s;
    release.artists = {u"Okuma Drift"_s};
    release.album = u"Night Signals"_s;
    release.albumArtists = {u"Okuma Drift"_s};
    release.releaseDate = u"2024-03-08"_s;
    release.trackNumber = 1;
    release.trackTotal = 2;
    release.durationMs = 3000;
    return release;
}

class AlbumServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
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

        selecta::core::TransferService::Options transferOptions;
        transferOptions.incomingDir = dir.filePath(u"incoming"_s);
        transferOptions.previewDir = dir.filePath(u"previews"_s);
        transferOptions.retryDelaysMs = {100, 100, 100};
        transferOptions.pacedRetryMs = 100;
        transfers = std::make_unique<selecta::core::TransferService>(db, governor, *network, log, transferOptions,
                                                                     [this] { return now; });

        selecta::core::LibraryConfig library;
        library.inboxRoot = dir.filePath(u"Inbox"_s);
        library.collectionRoots = {dir.filePath(u"Collection"_s)};
        ASSERT_TRUE(QDir().mkpath(library.inboxRoot));
        ASSERT_TRUE(QDir().mkpath(library.collectionRoots.constFirst()));
        inbox = std::make_unique<selecta::core::InboxService>(db, library, trash());

        AlbumService::Options options;
        options.stagingDir = dir.filePath(u"staging"_s);
        options.encodedBy = u"Selecta 9.9.9"_s;
        options.listingTimeout = 300ms;
        options.metadataTimeout = 1s;
        albums = std::make_unique<AlbumService>(db, governor, *network, *transfers, *inbox, log, trash(), options,
                                                [this] { return now; });
        QObject::connect(transfers.get(), &selecta::core::TransferService::completed, albums.get(),
                         &AlbumService::fileCompleted);
        QObject::connect(transfers.get(), &selecta::core::TransferService::changed, albums.get(),
                         &AlbumService::downloadChanged);
        QObject::connect(inbox.get(), &selecta::core::InboxService::trashed, albums.get(),
                         &AlbumService::inboxItemTrashed);
    }

    void TearDown() override
    {
        albums.reset();
        inbox.reset();
        if (transfers) {
            transfers->shutdown();
        }
        network.reset();
        transfers.reset();
    }

    selecta::library::FileOps::TrashFunction trash()
    {
        return [this](const QString &path, QString *to) {
            trashed.append(path);
            *to = dir.filePath(u"Trash/"_s + QFileInfo(path).fileName());
            QDir().mkpath(dir.filePath(u"Trash"_s));
            return QFile::rename(path, *to);
        };
    }

    // vinyl_ghost shares the album: two FLAC tracks, the cover and a rip log.
    std::unique_ptr<FakePeer> ghost()
    {
        auto peer = std::make_unique<FakePeer>(u"vinyl_ghost"_s, &server);
        EXPECT_TRUE(peer->start());
        peer->answerIndirect = false;
        selecta::slsk::SharedDirectory directory;
        directory.path = selecta::slsk::RemotePath{albumFolder()};
        const auto share = [&](const QByteArray &name, const QByteArray &bytes) {
            directory.files.append(selecta::slsk::FileEntry{.name = selecta::slsk::RemotePath{name},
                                                            .size = static_cast<quint64>(bytes.size()),
                                                            .extension = {},
                                                            .attributes = {}});
            peer->files.insert(albumFolder() + '\\' + name, FakePeer::Upload{.content = bytes});
        };
        share("01 - Okuma Drift - Neon Rain.flac", synth::makeFlac(synth::quantised(synth::sine(440.0, 3.0), 16)));
        share("02 - Okuma Drift - Low Tide.flac", synth::makeFlac(synth::quantised(synth::sine(330.0, 4.0), 16)));
        share("cover.jpg", jpeg(600));
        share("rip.log", "Exact Audio Copy V1.6");
        peer->shares.directories.append(directory);
        return peer;
    }

    static QList<SearchHit> searchHits(const FakePeer &peer)
    {
        QList<SearchHit> hits;
        for (auto it = peer.files.cbegin(); it != peer.files.cend(); ++it) {
            if (it.key().endsWith(".flac")) {
                hits.append(SearchHit{.peer = peer.username(),
                                      .path = it.key(),
                                      .size = FakePeer::sizeOf(it.value()),
                                      .extension = u"flac"_s,
                                      .attributes = {},
                                      .freeSlot = true,
                                      .queueLength = 0,
                                      .averageSpeed = 0});
            }
        }
        return hits;
    }

    bool finishes(qint64 albumId, std::chrono::milliseconds timeout = 30s)
    {
        return waitFor(
            [&] {
                const std::optional<AlbumDownload> album = albums->album(albumId);
                return album && album->isFinished();
            },
            timeout);
    }

    // A single already in the Inbox, downloaded as an MP3 320.
    qint64 inboxSingle(const QString &title, qint64 durationMs)
    {
        InboxItem item;
        item.kind = InboxItem::Kind::Single;
        item.relativePath = u"Singles/Okuma_Drift_-_%1.mp3"_s.arg(QString(title).replace(u' ', u'_'));
        item.state = InboxItem::State::Ready;
        item.artist = u"Okuma Drift"_s;
        item.title = title;
        item.durationMs = durationMs;
        item.format = u"MP3"_s;
        item.bitrate = 320;
        item.sourceFormat = u"MP3 320"_s;
        item.createdAt = now;
        EXPECT_TRUE(writeFile(dir.filePath(u"Inbox/"_s + item.relativePath), "single"));
        return inbox->add(item).value_or(0);
    }

    QList<InboxItem> inboxItems(InboxItem::Kind kind)
    {
        QList<InboxItem> items;
        const QList<InboxEntry> entries = inbox->entries();
        for (const InboxEntry &entry : entries) {
            if (entry.item.kind == kind) {
                items.append(entry.item);
            }
        }
        return items;
    }

    QTemporaryDir dir;
    FakeServer server;
    qint64 now = kNow;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock, {.today = {}, .random01 = [] { return 0.0; }}};
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    selecta::meta::InMemoryTokenStore tokens;
    QStringList trashed;
    std::unique_ptr<selecta::core::NetworkService> network;
    std::unique_ptr<selecta::core::TransferService> transfers;
    std::unique_ptr<selecta::core::InboxService> inbox;
    std::unique_ptr<AlbumService> albums;
};

} // namespace

TEST_F(AlbumServiceTest, DownloadsTheListedFolderIntoOneInboxAlbum)
{
    auto peer = ghost();
    QSignalSpy changes(albums.get(), &AlbumService::albumChanged);
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));

    const std::optional<AlbumDownload> album = albums->album(*id);
    EXPECT_EQ(album->state, AlbumDownload::State::Arrived) << album->message.toStdString();
    ASSERT_TRUE(album->inboxItem.has_value());

    // The peer was asked what the folder holds; its audio and cover came, its log didn't.
    EXPECT_EQ(peer->receivedOf<selecta::slsk::peer::FolderContentsRequest>().size(), 1);
    const QList<Download> downloads = selecta::core::DownloadStore(db).forAlbum(*id);
    ASSERT_EQ(downloads.size(), 3);
    for (const Download &download : downloads) {
        EXPECT_EQ(download.state, Download::State::Arrived) << download.remoteFileName().toStdString();
        EXPECT_EQ(download.inboxItem, album->inboxItem);
        EXPECT_FALSE(download.remoteFileName().endsWith(u".log"_s));
    }

    // One Inbox album with both tracks and the cover; each track in the history.
    const QList<InboxItem> items = inboxItems(InboxItem::Kind::Album);
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items.constFirst().id, *album->inboxItem);
    EXPECT_EQ(items.constFirst().trackCount, 2);
    EXPECT_EQ(items.constFirst().album, u"Night Signals"_s);
    const QDir folder(dir.filePath(u"Inbox/"_s + items.constFirst().relativePath));
    EXPECT_EQ(folder.entryList({u"*.mp3"_s}, QDir::Files).size(), 2);
    EXPECT_TRUE(folder.exists(u"cover.jpg"_s));
    const QList<selecta::library::HistoryEntry> history = selecta::library::HistoryStore(db).recent(10);
    ASSERT_EQ(history.size(), 2);
    for (const selecta::library::HistoryEntry &entry : history) {
        EXPECT_EQ(entry.kind, selecta::library::HistoryEntry::Kind::Album);
        EXPECT_EQ(entry.inboxItem, album->inboxItem);
        EXPECT_EQ(entry.quality, u"FLAC 16/44.1"_s);
    }

    // Listing, downloading, making, arrived: each step said.
    QList<AlbumDownload::State> states;
    for (const QList<QVariant> &change : std::as_const(changes)) {
        const auto state = change.at(0).value<AlbumDownload>().state;
        if (states.isEmpty() || states.constLast() != state) {
            states.append(state);
        }
    }
    EXPECT_EQ(states, (QList<AlbumDownload::State>{AlbumDownload::State::Listing, AlbumDownload::State::Downloading,
                                                   AlbumDownload::State::Converting, AlbumDownload::State::Arrived}));
    EXPECT_TRUE(albums->unfinished().isEmpty());
}

TEST_F(AlbumServiceTest, AnArrivalIsReportedOnce)
{
    auto peer = ghost();
    const QSignalSpy reported(albums.get(), &AlbumService::reported);
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    ASSERT_EQ(reported.size(), 1);
    const auto report = reported.at(0).at(0).value<AlbumReport>();
    EXPECT_EQ(report.albumId, *id);
    EXPECT_EQ(report.album, u"Night Signals"_s);
    EXPECT_TRUE(report.arrival);
    EXPECT_TRUE(report.complete);
    EXPECT_EQ(report.tracks, 2);
    EXPECT_TRUE(report.where.startsWith(u"Albums/"_s)) << report.where.toStdString();
    EXPECT_TRUE(report.warning.isEmpty());
    EXPECT_TRUE(report.trashed.isEmpty() && report.held.isEmpty() && report.flagged.isEmpty());
}

TEST_F(AlbumServiceTest, AFolderThePeerDoesntListComesFromTheSearchResults)
{
    auto peer = ghost();
    peer->ignoreRequests = true;
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), searchHits(*peer), neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    const std::optional<AlbumDownload> album = albums->album(*id);
    EXPECT_EQ(album->state, AlbumDownload::State::Arrived) << album->message.toStdString();
    EXPECT_EQ(selecta::core::DownloadStore(db).forAlbum(*id).size(), 2); // only the files the search found
}

TEST_F(AlbumServiceTest, AFileThatNeverComesMakesAPartialAlbum)
{
    auto peer = ghost();
    peer->files.remove(albumFolder() + "\\02 - Okuma Drift - Low Tide.flac"); // listed, but "File not shared."
    peer->queueBehaviour = FakePeer::QueueBehaviour::StartUpload;
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    const std::optional<AlbumDownload> album = albums->album(*id);
    EXPECT_EQ(album->state, AlbumDownload::State::Partial) << album->message.toStdString();
    EXPECT_TRUE(album->message.contains(u"1 file"_s)) << album->message.toStdString();
    EXPECT_EQ(inboxItems(InboxItem::Kind::Album).constFirst().trackCount, 1);
}

TEST_F(AlbumServiceTest, ASingleInTheInboxGoesToTheTrashAndUndoPutsItBack)
{
    const qint64 single = inboxSingle(u"Neon Rain"_s, 3000);
    const qint64 other = inboxSingle(u"Another Track"_s, 5000);
    ASSERT_GT(single, 0);
    auto peer = ghost();
    QSignalSpy reported(albums.get(), &AlbumService::reported);
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    ASSERT_TRUE(waitFor([&] { return !reported.isEmpty(); }));

    const auto report = reported.at(0).at(0).value<AlbumReport>();
    EXPECT_EQ(report.albumId, *id);
    EXPECT_EQ(report.album, u"Night Signals"_s);
    EXPECT_EQ(report.trashed, QStringList{u"Okuma Drift – Neon Rain"_s});
    EXPECT_TRUE(report.flagged.isEmpty());
    EXPECT_TRUE(report.held.isEmpty());

    // The single went; the unrelated one stayed.
    ASSERT_TRUE(waitFor([&] { return inboxItems(InboxItem::Kind::Single).size() == 1; }));
    EXPECT_EQ(inboxItems(InboxItem::Kind::Single).constFirst().id, other);
    const QString singlePath = dir.filePath(u"Inbox/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s);
    EXPECT_FALSE(QFileInfo::exists(singlePath));
    EXPECT_EQ(trashed.filter(u"Inbox/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s).size(), 1)
        << trashed.join(u", "_s).toStdString();

    // Undo: it's back, under its old name.
    albums->undoSupersede(*id);
    ASSERT_TRUE(waitFor([&] { return inboxItems(InboxItem::Kind::Single).size() == 2; }));
    EXPECT_TRUE(QFileInfo::exists(singlePath));
}

TEST_F(AlbumServiceTest, ManySinglesAtOnceWaitForAYes)
{
    // Every single in the Inbox is on the album, twice over: the circuit breaker asks first (T42).
    for (const QString &title : {u"Neon Rain"_s, u"Low Tide"_s}) {
        const qint64 durationMs = title == u"Neon Rain"_s ? 3000 : 4000;
        const qint64 single = inboxSingle(title, durationMs);
        ASSERT_GT(single, 0);
        InboxItem copy = *inbox->item(single);
        copy.id = 0;
        copy.relativePath.replace(u".mp3"_s, u" (2).mp3"_s);
        ASSERT_TRUE(writeFile(dir.filePath(u"Inbox/"_s + copy.relativePath), "single"));
        ASSERT_TRUE(inbox->add(copy).has_value());
    }
    ASSERT_EQ(inboxItems(InboxItem::Kind::Single).size(), 4);
    auto peer = ghost();
    QSignalSpy reported(albums.get(), &AlbumService::reported);
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    ASSERT_TRUE(waitFor([&] { return !reported.isEmpty(); }, 15s));
    const auto asked = reported.at(0).at(0).value<AlbumReport>();
    EXPECT_TRUE(asked.trashed.isEmpty());
    EXPECT_EQ(asked.held.size(), 4);
    QTest::qWait(100);
    EXPECT_EQ(inboxItems(InboxItem::Kind::Single).size(), 4); // nothing moved yet
    // Meanwhile each one says what album it's on (DL-8).
    const QList<InboxEntry> entries = inbox->entries();
    for (const InboxEntry &entry : entries) {
        if (entry.item.kind == InboxItem::Kind::Single) {
            EXPECT_EQ(entry.alsoOn, QStringList{u"Night Signals"_s}) << entry.item.relativePath.toStdString();
        }
    }

    // Yes: they go, and the report says so (with Undo in the app).
    albums->confirmSupersede(*id);
    ASSERT_EQ(reported.size(), 2);
    const auto confirmed = reported.at(1).at(0).value<AlbumReport>();
    EXPECT_EQ(confirmed.trashed.size(), 4);
    EXPECT_TRUE(confirmed.held.isEmpty());
    ASSERT_TRUE(waitFor([&] { return inboxItems(InboxItem::Kind::Single).isEmpty(); }, 15s));
    EXPECT_TRUE(selecta::core::SupersedeFlagStore(db).all().isEmpty()); // the flags went with the singles
    albums->undoSupersede(*id);
    ASSERT_TRUE(waitFor([&] { return inboxItems(InboxItem::Kind::Single).size() == 4; }, 15s));

    // Asked once: a second yes does nothing.
    albums->confirmSupersede(*id);
    EXPECT_EQ(reported.size(), 2);
}

TEST_F(AlbumServiceTest, ASingleFiledInTheCollectionIsOnlyFlagged)
{
    // The collection's Singles folder holds the same track.
    const QString filed = QDir(dir.filePath(u"Collection/Singles"_s)).filePath(u"Okuma Drift - Neon Rain.mp3"_s);
    ASSERT_TRUE(writeFile(filed, "filed"));
    selecta::collection::ScannedFile file;
    file.file.path = filed;
    file.file.size = 5;
    file.artist = u"Okuma Drift"_s;
    file.title = u"Neon Rain"_s;
    file.durationMs = 3000;
    file.quality.codec = selecta::match::Codec::Mp3;
    file.quality.bitrateKbps = 320;
    ASSERT_TRUE(selecta::core::CollectionStore(db).apply({file}, {}, {}));

    auto peer = ghost();
    QSignalSpy reported(albums.get(), &AlbumService::reported);
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(finishes(*id));
    ASSERT_TRUE(waitFor([&] { return !reported.isEmpty(); }));
    const auto report = reported.at(0).at(0).value<AlbumReport>();
    EXPECT_TRUE(report.trashed.isEmpty());
    EXPECT_EQ(report.flagged, QStringList{u"Okuma Drift – Neon Rain"_s});
    EXPECT_TRUE(QFileInfo::exists(filed)); // never touched
    EXPECT_TRUE(trashed.filter(u"Collection"_s).isEmpty());

    // The pair is kept, for the Collection view (DL-8).
    const QList<selecta::core::SupersedeFlag> flags = selecta::core::SupersedeFlagStore(db).all();
    ASSERT_EQ(flags.size(), 1);
    EXPECT_EQ(flags.constFirst().singlePath, filed);
    EXPECT_FALSE(flags.constFirst().singleItem.has_value());
    EXPECT_EQ(flags.constFirst().album, u"Night Signals"_s);
    EXPECT_EQ(flags.constFirst().single, u"Okuma Drift – Neon Rain"_s);
}

TEST_F(AlbumServiceTest, CancellingStopsTheAlbumAndItsDownloads)
{
    auto peer = ghost();
    peer->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(waitFor([&] { return selecta::core::DownloadStore(db).forAlbum(*id).size() == 3; }));
    albums->cancel(*id);
    EXPECT_EQ(albums->album(*id)->state, AlbumDownload::State::Cancelled);
    ASSERT_TRUE(waitFor([&] {
        const QList<Download> downloads = selecta::core::DownloadStore(db).forAlbum(*id);
        return std::ranges::all_of(downloads, [](const Download &d) { return d.state == Download::State::Cancelled; });
    }));
    EXPECT_TRUE(inboxItems(InboxItem::Kind::Album).isEmpty());
}

TEST_F(AlbumServiceTest, AnUnfinishedAlbumGoesOnAfterARestart)
{
    auto peer = ghost();
    peer->queueBehaviour = FakePeer::QueueBehaviour::Hold;
    const std::optional<qint64> id = albums->download(peer->username(), albumFolder(), {}, neonRain());
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(waitFor([&] { return peer->heldCount() == 3; }));

    // A quit: the services go, the rows stay. Then Selecta starts again and the peer sends the files.
    albums.reset();
    transfers->shutdown();
    transfers.reset();
    selecta::core::TransferService::Options transferOptions;
    transferOptions.incomingDir = dir.filePath(u"incoming"_s);
    transferOptions.previewDir = dir.filePath(u"previews"_s);
    transferOptions.retryDelaysMs = {100, 100, 100};
    transferOptions.pacedRetryMs = 100;
    transfers = std::make_unique<selecta::core::TransferService>(db, governor, *network, log, transferOptions,
                                                                 [this] { return now; });
    AlbumService::Options options;
    options.stagingDir = dir.filePath(u"staging"_s);
    options.metadataTimeout = 1s;
    albums = std::make_unique<AlbumService>(db, governor, *network, *transfers, *inbox, log, trash(), options,
                                            [this] { return now; });
    QObject::connect(transfers.get(), &selecta::core::TransferService::completed, albums.get(),
                     &AlbumService::fileCompleted);
    QObject::connect(transfers.get(), &selecta::core::TransferService::changed, albums.get(),
                     &AlbumService::downloadChanged);
    transfers->load();
    albums->restore();
    EXPECT_EQ(albums->unfinished().size(), 1);

    peer->queueBehaviour = FakePeer::QueueBehaviour::StartUpload;
    ASSERT_TRUE(waitFor([&] { return peer->heldCount() > 0; }, 10s)); // queued again
    const QList<QByteArray> names{"01 - Okuma Drift - Neon Rain.flac", "02 - Okuma Drift - Low Tide.flac", "cover.jpg"};
    for (const QByteArray &name : names) {
        peer->startUpload(u"selecta_test"_s, selecta::slsk::RemotePath{albumFolder() + '\\' + name});
    }
    ASSERT_TRUE(finishes(*id));
    EXPECT_EQ(albums->album(*id)->state, AlbumDownload::State::Arrived) << albums->album(*id)->message.toStdString();
}

TEST(SupersedeFlagStore, KeepsOneFlagPerPairAndDropsItWithTheSingle)
{
    selecta::storage::Database db;
    ASSERT_TRUE(db.open(u":memory:"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    InboxItem single;
    single.relativePath = u"Singles/a.mp3"_s;
    single.state = InboxItem::State::Ready;
    const std::optional<qint64> item = selecta::library::InboxStore(db).add(single);
    ASSERT_TRUE(item.has_value());

    selecta::core::SupersedeFlagStore store(db);
    selecta::core::SupersedeFlag flag;
    flag.album = u"Night Signals"_s;
    flag.singleItem = item;
    flag.single = u"Okuma Drift – Neon Rain"_s;
    flag.createdAt = 1000;
    const std::optional<qint64> first = store.add(flag);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(store.add(flag), first); // the same pair again
    selecta::core::SupersedeFlag filed;
    filed.album = u"Night Signals"_s;
    filed.singlePath = u"/Music/Singles/Okuma Drift - Neon Rain.mp3"_s;
    filed.single = u"Okuma Drift – Neon Rain"_s;
    filed.createdAt = 2000;
    const std::optional<qint64> second = store.add(filed);
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(second, first);

    EXPECT_EQ(store.all().size(), 2);
    EXPECT_EQ(store.all().constFirst().id, *second); // newest first
    EXPECT_EQ(store.albumsByInboxItem().value(*item), QStringList{u"Night Signals"_s});

    ASSERT_TRUE(selecta::library::InboxStore(db).remove(*item));
    EXPECT_EQ(store.all().size(), 1); // the Inbox single's flag went with it
    EXPECT_TRUE(store.remove(*second));
    EXPECT_TRUE(store.all().isEmpty());
}
