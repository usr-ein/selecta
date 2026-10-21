// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CollectionService.h"

#include "SyntheticAudio.h"
#include "convert/TagWriter.h"
#include "core/CollectionStore.h"
#include "core/GetTracksService.h"
#include "match/Matcher.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

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
                const QString &album = {})
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
