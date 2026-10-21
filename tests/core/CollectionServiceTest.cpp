// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CollectionService.h"

#include "SyntheticAudio.h"
#include "convert/TagWriter.h"
#include "core/CollectionStore.h"
#include "core/GetTracksService.h"
#include "match/Matcher.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::collection::FolderRoles;
using selecta::core::CollectionService;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;

namespace {

bool writeTrack(const QString &path, const QString &artist, const QString &title, double seconds = 4.0,
                const QString &album = {}, const QString &genre = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray bytes = path.endsWith(u".flac"_s)
                                 ? synth::makeFlac(synth::quantised(synth::sine(440.0, seconds), 16))
                                 : synth::makeMp3(synth::sine(440.0, seconds));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return false;
    }
    file.close();
    selecta::convert::MetadataModel tags;
    tags.set(keys::Artist, artist, selecta::convert::Provenance::Embedded);
    tags.set(keys::Title, title, selecta::convert::Provenance::Embedded);
    if (!album.isEmpty()) {
        tags.set(keys::Album, album, selecta::convert::Provenance::Embedded);
    }
    if (!genre.isEmpty()) {
        tags.set(keys::Genre, genre, selecta::convert::Provenance::Embedded);
    }
    return selecta::convert::writeTags(path, tags).ok();
}

class CollectionServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        root = dir.filePath(u"Music"_s);
        tracks = root + u"/Tracks"_s;
        albums = root + u"/Albums"_s;
        ASSERT_TRUE(writeTrack(tracks + u"/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s));
        ASSERT_TRUE(writeTrack(tracks + u"/Halden Rue - Tidewater.flac"_s, u"Halden Rue"_s, u"Tidewater"_s));
        ASSERT_TRUE(QDir().mkpath(albums));
        service = std::make_unique<CollectionService>(
            db, CollectionService::Options{.readers = 2, .watch = false, .rebuildDelayMs = 10},
            [] { return qint64{1'800'000'000'000}; });
    }

    // The collection's best match for a track, if any.
    std::optional<selecta::match::RecordMatch> lookUp(const QString &artist, const QString &title) const
    {
        selecta::match::LocalTrackQuery query;
        query.artist = artist;
        query.title = title;
        const QList<selecta::match::RecordMatch> matches =
            selecta::match::Matcher(service->index()).matchLocal(query, 4);
        if (matches.isEmpty() || matches.constFirst().tier == selecta::match::Tier::None) {
            return std::nullopt;
        }
        return matches.constFirst();
    }

    // The tracks folder and the albums folder, side by side.
    FolderRoles roles() const { return {tracks, albums}; }

    QTemporaryDir dir;
    QString root;
    QString tracks;
    QString albums;
    selecta::storage::Database db;
    std::unique_ptr<CollectionService> service;
};

} // namespace

TEST_F(CollectionServiceTest, ScansTheFoldersIntoTheIndex)
{
    const QSignalSpy indexes(service.get(), &CollectionService::indexChanged);
    service->setFolders(roles(), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
    EXPECT_FALSE(service->status().scanning);
    EXPECT_GT(service->status().lastScanMs, 0);
    ASSERT_TRUE(service->index());
    EXPECT_FALSE(indexes.isEmpty());
    const auto found = lookUp(u"Okuma Drift"_s, u"Neon Rain"_s);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, selecta::match::Source::Collection);
    EXPECT_FALSE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());

    // LIB-5: what the collection is made of.
    const selecta::core::CollectionStatus status = service->status();
    EXPECT_EQ(status.bytes, QFileInfo(tracks + u"/Okuma Drift - Neon Rain.mp3"_s).size() +
                                QFileInfo(tracks + u"/Halden Rue - Tidewater.flac"_s).size());
    EXPECT_EQ(status.roots, (QStringList{tracks, albums}));
    EXPECT_EQ(status.albums, 0); // loose tracks only
    ASSERT_EQ(status.formats.size(), 2);
    EXPECT_EQ(status.formats.at(0).second + status.formats.at(1).second, 2);
    QStringList names{status.formats.at(0).first, status.formats.at(1).first};
    names.sort();
    EXPECT_EQ(names.constLast(), u"MP3 320"_s) << names.join(u", "_s).toStdString(); // the synthetic MP3s are 320
    EXPECT_EQ(names.constFirst(), u"FLAC"_s);
}

TEST_F(CollectionServiceTest, ARescanFollowsChangesAndDeletions)
{
    service->setFolders(roles(), {});
    service->waitForIdle();
    ASSERT_TRUE(
        writeTrack(albums + u"/New/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    ASSERT_TRUE(QFile::remove(tracks + u"/Halden Rue - Tidewater.flac"_s));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
    EXPECT_EQ(service->status().albums, 1);
    EXPECT_TRUE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());
    EXPECT_FALSE(lookUp(u"Halden Rue"_s, u"Tidewater"_s).has_value());
}

TEST_F(CollectionServiceTest, AMissingFolderKeepsItsTracks)
{
    service->setFolders(roles(), {});
    service->waitForIdle();
    ASSERT_TRUE(QDir(root).rename(tracks, tracks + u"-unplugged"_s));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2); // an unplugged drive isn't a deletion
    EXPECT_TRUE(service->status().message.contains(u"Not there"_s)) << service->status().message.toStdString();
    EXPECT_TRUE(lookUp(u"Okuma Drift"_s, u"Neon Rain"_s).has_value());
}

TEST_F(CollectionServiceTest, FoldersRemovedFromSettingsAreForgotten)
{
    service->setFolders(roles(), {});
    service->waitForIdle();
    service->setFolders(FolderRoles(), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 0);
    EXPECT_EQ(selecta::core::CollectionStore(db).count(), 0);
    EXPECT_FALSE(lookUp(u"Okuma Drift"_s, u"Neon Rain"_s).has_value());
}

TEST_F(CollectionServiceTest, SelectasOwnFoldersInsideTheCollectionArentPartOfIt)
{
    const QString inbox = albums + u"/Selecta Inbox"_s;
    ASSERT_TRUE(
        writeTrack(inbox + u"/Singles/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    service->setFolders(roles(), {inbox});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
    EXPECT_EQ(service->status().albums, 0); // not an album folder either
    EXPECT_FALSE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());
}

TEST_F(CollectionServiceTest, InboxItemsAreInTheIndexAsInbox)
{
    selecta::library::InboxItem item;
    item.id = 7;
    item.artist = u"Kassia Moreau"_s;
    item.title = u"Night Transit"_s;
    item.format = u"MP3"_s;
    item.bitrate = 320;
    service->setFolders(roles(), {});
    service->setInbox({item});
    service->waitForIdle();
    const auto found = lookUp(u"Kassia Moreau"_s, u"Night Transit"_s);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, selecta::match::Source::Inbox);
    EXPECT_EQ(found->id, 7U);
}

TEST_F(CollectionServiceTest, GetTracksLinesYouHaveAreMarkedAndUnticked)
{
    service->setFolders(roles(), {});
    service->waitForIdle();
    selecta::core::GetTracksService getTracks(db, [] { return qint64{1'800'000'000'000}; });
    getTracks.add(u"Okuma Drift - Neon Rain\nKassia Moreau - Night Transit"_s);
    getTracks.setLibraryIndex(service->index());
    const QList<selecta::core::GetLine> lines = getTracks.lines();
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines.at(0).findState, selecta::core::GetLine::FindState::Owned);
    EXPECT_FALSE(lines.at(0).ticked);
    EXPECT_FALSE(lines.at(0).message.isEmpty());
    EXPECT_EQ(lines.at(1).findState, selecta::core::GetLine::FindState::Idle);

    // The copy goes away: the line goes back to be found.
    ASSERT_TRUE(QFile::remove(tracks + u"/Okuma Drift - Neon Rain.mp3"_s));
    service->scanNow();
    service->waitForIdle();
    getTracks.setLibraryIndex(service->index());
    EXPECT_EQ(getTracks.lines().at(0).findState, selecta::core::GetLine::FindState::Idle);
}

// The beta's "4 albums in ~/Music": loose tracks, three of them from one release, and an empty albums folder.
TEST_F(CollectionServiceTest, LooseTracksAreNeverCountedAsAlbums)
{
    for (int take = 1; take <= 3; ++take) {
        ASSERT_TRUE(writeTrack(tracks + u"/Beau Didier - Take %1.mp3"_s.arg(take), u"Beau Didier"_s,
                               u"Take %1"_s.arg(take), 2.0, u"Take EP"_s));
    }
    service->setFolders(roles(), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 5);
    EXPECT_EQ(service->status().albums, 0);
    ASSERT_TRUE(service->index());
    EXPECT_EQ(service->index()->albumCount(), 0);

    // An album folder makes one album, whatever its tracks' tags say.
    ASSERT_TRUE(writeTrack(albums + u"/Bicep - Isles/01 Atlas.mp3"_s, u"Bicep"_s, u"Atlas"_s, 2.0));
    ASSERT_TRUE(writeTrack(albums + u"/Bicep - Isles/02 Saku.mp3"_s, u"Bicep"_s, u"Saku"_s, 2.0));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 7);
    EXPECT_EQ(service->status().albums, 1);
    EXPECT_EQ(service->index()->albumCount(), 1);
}

TEST_F(CollectionServiceTest, TheRolesDecideWhatIsInTheCollection)
{
    ASSERT_TRUE(
        writeTrack(tracks + u"/Old/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    ASSERT_TRUE(writeTrack(albums + u"/Marlo Vance - Tidal Memory.mp3"_s, u"Marlo Vance"_s, u"Tidal Memory"_s));
    service->setFolders(roles(), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2); // a subfolder of the tracks folder and a file lying in the albums folder
    EXPECT_FALSE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());
    EXPECT_FALSE(lookUp(u"Marlo Vance"_s, u"Tidal Memory"_s).has_value());
}

// After the update, an index made from one folder holding everything keeps only what the two folders' roles hold.
TEST_F(CollectionServiceTest, AnIndexOfTheOldSingleFolderKeepsWhatTheRolesHold)
{
    ASSERT_TRUE(
        writeTrack(root + u"/Elsewhere/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    ASSERT_TRUE(writeTrack(tracks + u"/Old/Marlo Vance - Tidal Memory.mp3"_s, u"Marlo Vance"_s, u"Tidal Memory"_s));
    ASSERT_TRUE(writeTrack(albums + u"/Bicep - Isles/01 Atlas.mp3"_s, u"Bicep"_s, u"Atlas"_s));
    // The old setting: ~/Music as a whole (a folder with both roles scans everything, as the single folder did).
    service->setFolders(FolderRoles(root, root), {});
    service->waitForIdle();
    ASSERT_EQ(service->status().tracks, 5);

    service->setFolders(roles(), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 3); // the two loose tracks and the album's track
    EXPECT_EQ(selecta::core::CollectionStore(db).count(), 3);
    EXPECT_EQ(service->status().albums, 1);
    EXPECT_FALSE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());
    EXPECT_FALSE(lookUp(u"Marlo Vance"_s, u"Tidal Memory"_s).has_value());
    EXPECT_TRUE(lookUp(u"Bicep"_s, u"Atlas"_s).has_value());
}

TEST_F(CollectionServiceTest, SwappingTheRolesScansAgain)
{
    ASSERT_TRUE(writeTrack(albums + u"/Bicep - Isles/01 Atlas.mp3"_s, u"Bicep"_s, u"Atlas"_s));
    service->setFolders(roles(), {});
    service->waitForIdle();
    ASSERT_EQ(service->status().tracks, 3);
    ASSERT_EQ(service->status().albums, 1);
    // The same two folders, roles swapped: loose files lie in the albums folder, the album is a tracks subfolder.
    service->setFolders(FolderRoles(albums, tracks), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 0);
    EXPECT_EQ(service->status().albums, 0);
    EXPECT_EQ(service->status().roots, (QStringList{albums, tracks}));
}

TEST_F(CollectionServiceTest, OneFolderChosenIsEnoughToScan)
{
    service->setFolders(FolderRoles(tracks, {}), {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
    EXPECT_EQ(service->status().roots, QStringList{tracks});
}

// --- The Collection view's listing (P-010) ---------------------------------------------------------------------------

namespace {

const selecta::core::CollectionTrack *trackAt(const selecta::core::CollectionListing &listing, const QString &path)
{
    const auto it = std::ranges::find_if(listing.tracks, [&path](const selecta::core::CollectionTrack &track) {
        return QDir::cleanPath(track.path) == QDir::cleanPath(path);
    });
    return it == listing.tracks.cend() ? nullptr : &*it;
}

selecta::library::HistoryEntry filedDownload(qint64 id, const QString &filedPath, qint64 completedAt,
                                             const QString &quality, std::optional<int> likelySourceKbps = {})
{
    selecta::library::HistoryEntry entry;
    entry.id = id;
    entry.completedAt = completedAt;
    entry.peer = u"vinyl_ghost"_s;
    entry.remotePath = "@@vinyl\\track"_ba;
    entry.quality = quality;
    entry.whereabouts = selecta::library::HistoryEntry::Whereabouts::Filed;
    entry.filedPath = filedPath;
    entry.likelySourceKbps = likelySourceKbps;
    return entry;
}

} // namespace

TEST_F(CollectionServiceTest, TheListingHasEveryTrackWithItsAlbumTierAndGenre)
{
    const QString nightfall = albums + u"/Okuma Drift - Nightfall"_s;
    ASSERT_TRUE(writeTrack(nightfall + u"/02 Dawn.mp3"_s, u"Okuma Drift"_s, u"Dawn"_s, 4.0, u"Nightfall"_s));
    ASSERT_TRUE(
        writeTrack(nightfall + u"/01 Dusk.mp3"_s, u"Okuma Drift"_s, u"Dusk"_s, 4.0, u"Nightfall"_s, u"Ambient"_s));
    ASSERT_TRUE(
        writeTrack(tracks + u"/Aster Vale - Glass.mp3"_s, u"Aster Vale"_s, u"Glass"_s, 4.0, {}, u"Deep House"_s));
    const QSignalSpy listings(service.get(), &CollectionService::listingChanged);
    service->setFolders(roles(), {});
    service->waitForIdle();
    EXPECT_FALSE(listings.isEmpty());
    const std::shared_ptr<const selecta::core::CollectionListing> listing = service->listing();
    ASSERT_TRUE(listing);
    ASSERT_EQ(listing->tracks.size(), 5);
    ASSERT_EQ(listing->albums.size(), 1);
    const selecta::core::CollectionAlbum &album = listing->albums.constFirst();
    EXPECT_EQ(album.title, u"Nightfall"_s);
    EXPECT_EQ(album.artist, u"Okuma Drift"_s);
    EXPECT_EQ(QDir::cleanPath(album.folder), QDir::cleanPath(nightfall));
    EXPECT_NE(album.id, 0U);

    const auto *dusk = trackAt(*listing, nightfall + u"/01 Dusk.mp3"_s);
    const auto *dawn = trackAt(*listing, nightfall + u"/02 Dawn.mp3"_s);
    const auto *glass = trackAt(*listing, tracks + u"/Aster Vale - Glass.mp3"_s);
    const auto *tidewater = trackAt(*listing, tracks + u"/Halden Rue - Tidewater.flac"_s);
    ASSERT_TRUE(dusk && dawn && glass && tidewater);
    EXPECT_EQ(dusk->albumId, album.id);
    EXPECT_EQ(dawn->albumId, album.id);
    EXPECT_EQ(glass->albumId, 0U); // a loose track
    EXPECT_EQ(dusk->genre, u"Ambient"_s);
    EXPECT_EQ(glass->genre, u"Deep House"_s);
    EXPECT_TRUE(dawn->genre.isEmpty());
    EXPECT_EQ(glass->tier, selecta::core::QualityTier::Top); // the synthetic MP3s are 320
    EXPECT_EQ(tidewater->tier, selecta::core::QualityTier::Lossless);
    EXPECT_EQ(glass->title, u"Glass"_s);
    EXPECT_GT(glass->sizeBytes, 0);
    EXPECT_GT(glass->durationMs, 0);
    // Not downloaded by Selecta: as new as the file.
    EXPECT_EQ(glass->addedMs, QFileInfo(tracks + u"/Aster Vale - Glass.mp3"_s).lastModified().toMSecsSinceEpoch());
    // The view names each folder by its role.
    EXPECT_EQ(service->status().tracksFolder, tracks);
    EXPECT_EQ(service->status().albumsFolder, albums);
}

// History says when the files Selecta filed came and what they came from. It keeps the paths FileOps reported, real
// ones, while the folders may be chosen through a symbolic link (/var is one on macOS).
TEST_F(CollectionServiceTest, HistorySaysWhenAFiledTrackCameAndWhatItCameFrom)
{
    const QString linked = dir.filePath(u"Linked"_s);
    ASSERT_TRUE(QFile::link(root, linked));
    const QString realTracks = QFileInfo(tracks).canonicalFilePath();
    const QString realAlbums = QFileInfo(albums).canonicalFilePath();
    ASSERT_FALSE(realTracks.isEmpty());
    const QString nightfall = albums + u"/Okuma Drift - Nightfall"_s;
    ASSERT_TRUE(writeTrack(nightfall + u"/01 Dusk.mp3"_s, u"Okuma Drift"_s, u"Dusk"_s, 4.0, u"Nightfall"_s));
    ASSERT_TRUE(writeTrack(nightfall + u"/02 Dawn.mp3"_s, u"Okuma Drift"_s, u"Dawn"_s, 4.0, u"Nightfall"_s));
    ASSERT_TRUE(writeTrack(tracks + u"/Aster Vale - Glass.mp3"_s, u"Aster Vale"_s, u"Glass"_s));
    service->setHistory({
        // An MP3 Selecta made from a FLAC download.
        filedDownload(1, realTracks + u"/Okuma Drift - Neon Rain.mp3"_s, 1'700'000'000'000, u"FLAC 16/44.1"_s),
        // An album made from FLAC files: one entry per file, filed as its folder.
        filedDownload(2, realAlbums + u"/Okuma Drift - Nightfall"_s, 1'700'000'100'000, u"FLAC 16/44.1"_s),
        filedDownload(3, realAlbums + u"/Okuma Drift - Nightfall"_s, 1'700'000'200'000, u"FLAC 24/96"_s),
        // A file filed once, then replaced by an upgrade the spectrum check found to be a fake 320: the newest says.
        filedDownload(4, realTracks + u"/Aster Vale - Glass.mp3"_s, 1'600'000'000'000, u"FLAC 16/44.1"_s),
        filedDownload(5, realTracks + u"/Aster Vale - Glass.mp3"_s, 1'700'000'300'000, u"MP3 320"_s, 128),
    });
    service->setFolders(FolderRoles(linked + u"/Tracks"_s, linked + u"/Albums"_s), {});
    service->waitForIdle();
    const std::shared_ptr<const selecta::core::CollectionListing> listing = service->listing();
    ASSERT_TRUE(listing);
    ASSERT_EQ(listing->tracks.size(), 5);

    const auto *neonRain = trackAt(*listing, linked + u"/Tracks/Okuma Drift - Neon Rain.mp3"_s);
    const auto *dusk = trackAt(*listing, linked + u"/Albums/Okuma Drift - Nightfall/01 Dusk.mp3"_s);
    const auto *glass = trackAt(*listing, linked + u"/Tracks/Aster Vale - Glass.mp3"_s);
    const auto *tidewater = trackAt(*listing, linked + u"/Tracks/Halden Rue - Tidewater.flac"_s);
    ASSERT_TRUE(neonRain && dusk && glass && tidewater);
    EXPECT_EQ(neonRain->tier, selecta::core::QualityTier::Lossless);
    EXPECT_EQ(neonRain->addedMs, 1'700'000'000'000);
    EXPECT_EQ(dusk->tier, selecta::core::QualityTier::Lossless);
    EXPECT_EQ(dusk->addedMs, 1'700'000'200'000); // the album's newest file
    EXPECT_EQ(glass->tier, selecta::core::QualityTier::Below192);
    EXPECT_EQ(glass->addedMs, 1'700'000'300'000);
    EXPECT_EQ(tidewater->tier, selecta::core::QualityTier::Lossless); // a FLAC Selecta didn't download
    EXPECT_EQ(tidewater->addedMs,
              QFileInfo(tracks + u"/Halden Rue - Tidewater.flac"_s).lastModified().toMSecsSinceEpoch());
}

// An album of lossy downloads isn't "from lossless", even when one of its files was.
TEST_F(CollectionServiceTest, AnAlbumIsFromLosslessOnlyWhenAllItsDownloadsWere)
{
    const QString nightfall = albums + u"/Okuma Drift - Nightfall"_s;
    ASSERT_TRUE(writeTrack(nightfall + u"/01 Dusk.mp3"_s, u"Okuma Drift"_s, u"Dusk"_s, 4.0, u"Nightfall"_s));
    const QString realAlbum = QFileInfo(nightfall).canonicalFilePath();
    service->setHistory({filedDownload(1, realAlbum, 1'700'000'000'000, u"FLAC 16/44.1"_s),
                         filedDownload(2, realAlbum, 1'700'000'000'001, u"MP3 320"_s)});
    service->setFolders(roles(), {});
    service->waitForIdle();
    const auto *dusk = trackAt(*service->listing(), nightfall + u"/01 Dusk.mp3"_s);
    ASSERT_NE(dusk, nullptr);
    EXPECT_EQ(dusk->tier, selecta::core::QualityTier::Top);
    EXPECT_EQ(dusk->addedMs, 1'700'000'000'001);
}

// The Inbox changes all the time while downloads arrive: the index is built again, the listing only when what it says
// changed, so the wall keeps its covers and its place.
TEST_F(CollectionServiceTest, AListingIsPublishedOnlyWhenItChanges)
{
    service->setFolders(roles(), {});
    service->waitForIdle();
    const std::shared_ptr<const selecta::core::CollectionListing> first = service->listing();
    ASSERT_TRUE(first);
    const QSignalSpy indexes(service.get(), &CollectionService::indexChanged);
    const QSignalSpy listings(service.get(), &CollectionService::listingChanged);
    selecta::library::InboxItem item;
    item.id = 7;
    item.artist = u"Kassia Moreau"_s;
    item.title = u"Night Transit"_s;
    service->setInbox({item});
    service->waitForIdle();
    EXPECT_EQ(indexes.size(), 1);
    EXPECT_TRUE(listings.isEmpty());
    EXPECT_EQ(service->listing(), first);

    // A file that changes is another listing.
    ASSERT_TRUE(writeTrack(tracks + u"/Aster Vale - Glass.mp3"_s, u"Aster Vale"_s, u"Glass"_s));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(listings.size(), 1);
    EXPECT_NE(service->listing(), first);
    EXPECT_EQ(service->listing()->tracks.size(), 3);
}
