// SPDX-License-Identifier: GPL-3.0-or-later
// Album folders among scanned files (note §8 step 1): tags, folder names, disc folders, various artists, stable ids.
#include "collection/Albums.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::collection::albumsOf;
using selecta::collection::FolderRoles;
using selecta::collection::groupAlbums;
using selecta::collection::ScannedFile;
using selecta::match::LibraryAlbum;

namespace {

ScannedFile file(const QString &path, const QString &artist, const QString &title, const QString &album = {},
                 int track = 0, const QString &albumArtist = {}, int year = 0)
{
    ScannedFile scanned;
    scanned.file = {path, 1000, 1000};
    scanned.artist = artist;
    scanned.title = title;
    scanned.album = album;
    scanned.trackNumber = track;
    scanned.albumArtist = albumArtist;
    scanned.year = year;
    return scanned;
}

QList<ScannedFile> islesFolder(const QString &folder = u"/Music/Bicep/Isles"_s)
{
    return {file(folder + u"/01 Atlas.mp3"_s, u"Bicep"_s, u"Atlas"_s, u"Isles"_s, 1, u"Bicep"_s, 2021),
            file(folder + u"/02 Cazenove.mp3"_s, u"Bicep"_s, u"Cazenove"_s, u"Isles"_s, 2, u"Bicep"_s, 2021),
            file(folder + u"/03 Apricots.mp3"_s, u"Bicep"_s, u"Apricots"_s, u"Isles"_s, 3, u"Bicep"_s, 2021)};
}

TEST(Albums, ThreeTracksSharingAnAlbumTagMakeAnAlbum)
{
    QHash<QString, quint64> ids;
    const QList<ScannedFile> files = islesFolder();
    const QList<LibraryAlbum> albums = groupAlbums(files, &ids);
    ASSERT_EQ(albums.size(), 1);
    const LibraryAlbum &album = albums.front();
    EXPECT_EQ(album.title, u"Isles"_s);
    EXPECT_EQ(album.artist, u"Bicep"_s);
    EXPECT_EQ(album.year, 2021);
    EXPECT_FALSE(album.variousArtists);
    EXPECT_EQ(album.source, selecta::match::Source::Collection);
    EXPECT_NE(album.id, 0U);
    ASSERT_EQ(ids.size(), 3);
    for (const ScannedFile &scanned : files) {
        EXPECT_EQ(ids.value(scanned.file.path), album.id);
    }
}

TEST(Albums, TwoTaggedTracksAreNotEnough)
{
    QList<ScannedFile> files = islesFolder();
    files.removeLast();
    QHash<QString, quint64> ids;
    EXPECT_TRUE(groupAlbums(files, &ids).isEmpty());
    EXPECT_TRUE(ids.isEmpty());
}

TEST(Albums, AFolderNamedLikeAnAlbumIsOne)
{
    const QString folder = u"/Music/Bicep - Isles (2021)"_s;
    const QList<ScannedFile> files{file(folder + u"/01 - Atlas.flac"_s, u"Bicep"_s, u"Atlas"_s, {}, 1),
                                   file(folder + u"/02 - Cazenove.flac"_s, u"Bicep"_s, u"Cazenove"_s, {}, 2)};
    const QList<LibraryAlbum> albums = groupAlbums(files, nullptr);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(albums.front().title, u"Isles"_s);
    EXPECT_EQ(albums.front().artist, u"Bicep"_s);
    EXPECT_EQ(albums.front().year, 2021);
}

TEST(Albums, PlainFoldersOfSinglesAreNotAlbums)
{
    const QList<ScannedFile> files{
        file(u"/Music/Singles/Bicep - Glue.mp3"_s, u"Bicep"_s, u"Glue"_s),
        file(u"/Music/Singles/Daft Punk - Aerodynamic.mp3"_s, u"Daft Punk"_s, u"Aerodynamic"_s),
        file(u"/Music/Singles/Moderat - Bad Kingdom.mp3"_s, u"Moderat"_s, u"Bad Kingdom"_s, u"II"_s)};
    EXPECT_TRUE(groupAlbums(files, nullptr).isEmpty());
    // An artist folder with numbered tracks but no album naming isn't an album either.
    const QList<ScannedFile> artistFolder{file(u"/Music/Bicep/01 Glue.mp3"_s, u"Bicep"_s, u"Glue"_s, {}, 1),
                                          file(u"/Music/Bicep/02 Opal.mp3"_s, u"Bicep"_s, u"Opal"_s, {}, 2)};
    EXPECT_TRUE(groupAlbums(artistFolder, nullptr).isEmpty());
}

TEST(Albums, DiscFoldersMergeIntoTheirAlbum)
{
    const QString folder = u"/Music/Moderat - II"_s;
    QList<ScannedFile> files;
    for (const QString &disc : {u"CD1"_s, u"Disc 2"_s}) {
        for (int track = 1; track <= 2; ++track) {
            files.append(file(u"%1/%2/%3 Track.mp3"_s.arg(folder, disc).arg(track), u"Moderat"_s,
                              u"Track %1"_s.arg(track), u"II"_s, track));
        }
    }
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = groupAlbums(files, &ids);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(albums.front().title, u"II"_s);
    EXPECT_EQ(ids.size(), 4);
}

TEST(Albums, VariousArtistsAreMarked)
{
    const QString folder = u"/Music/Compilations/Summer"_s;
    QList<ScannedFile> byTag;
    QList<ScannedFile> byArtists;
    const QStringList artists{u"Bicep"_s, u"Moderat"_s, u"Daft Punk"_s, u"Bonobo"_s};
    for (int i = 0; i < 4; ++i) {
        byTag.append(file(u"%1/%2.mp3"_s.arg(folder).arg(i), artists.at(i), u"Song %1"_s.arg(i), u"Summer"_s, i + 1,
                          u"Various Artists"_s));
        byArtists.append(file(u"/Music/Mix/%1.mp3"_s.arg(i), artists.at(i), u"Song %1"_s.arg(i), u"Mix"_s, i + 1));
    }
    const QList<LibraryAlbum> tagged = groupAlbums(byTag, nullptr);
    ASSERT_EQ(tagged.size(), 1);
    EXPECT_TRUE(tagged.front().variousArtists);
    EXPECT_EQ(tagged.front().artist, u"Various Artists"_s);
    const QList<LibraryAlbum> varied = groupAlbums(byArtists, nullptr);
    ASSERT_EQ(varied.size(), 1);
    EXPECT_TRUE(varied.front().variousArtists); // four tracks, four artists
    // One featured guest doesn't make an album a compilation.
    QList<ScannedFile> guest = islesFolder();
    guest.append(file(u"/Music/Bicep/Isles/04 Saku.mp3"_s, u"Bicep & Clara La San"_s, u"Saku"_s, u"Isles"_s, 4));
    EXPECT_FALSE(groupAlbums(guest, nullptr).constFirst().variousArtists);
}

TEST(Albums, OtherAlbumsTracksInTheFolderAreNotMembers)
{
    QList<ScannedFile> files = islesFolder();
    files.append(file(u"/Music/Bicep/Isles/Glue.mp3"_s, u"Bicep"_s, u"Glue"_s, u"Bicep"_s));
    files.append(file(u"/Music/Bicep/Isles/Untagged.mp3"_s, u"Bicep"_s, u"Untagged"_s));
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = groupAlbums(files, &ids);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(ids.size(), 3); // the folder holds another album's track, so the untagged one stays out too
    EXPECT_FALSE(ids.contains(u"/Music/Bicep/Isles/Glue.mp3"_s));
    EXPECT_FALSE(ids.contains(u"/Music/Bicep/Isles/Untagged.mp3"_s));
}

TEST(Albums, IdsAreStableAcrossScans)
{
    const QList<ScannedFile> isles = islesFolder();
    const QList<ScannedFile> other = islesFolder(u"/Music/Bicep/Isles (Deluxe)"_s);
    const quint64 alone = groupAlbums(isles, nullptr).constFirst().id;
    const QList<ScannedFile> both = other + isles;
    const QList<LibraryAlbum> albums = groupAlbums(both, nullptr);
    ASSERT_EQ(albums.size(), 2);
    EXPECT_NE(albums.at(0).id, albums.at(1).id);
    EXPECT_TRUE(albums.at(0).id == alone || albums.at(1).id == alone); // the same folder, the same id
    EXPECT_EQ(groupAlbums(isles, nullptr).constFirst().id, alone);
}

TEST(Albums, AlbumsAreSortedByFolder)
{
    const QList<ScannedFile> files = islesFolder(u"/Music/Z"_s) + islesFolder(u"/Music/A"_s);
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = groupAlbums(files, &ids);
    ASSERT_EQ(albums.size(), 2);
    EXPECT_EQ(ids.value(u"/Music/A/01 Atlas.mp3"_s), albums.at(0).id);
    EXPECT_EQ(ids.value(u"/Music/Z/01 Atlas.mp3"_s), albums.at(1).id);
}

TEST(Albums, NothingInNothingOut)
{
    QHash<QString, quint64> ids;
    EXPECT_TRUE(groupAlbums({}, &ids).isEmpty());
    EXPECT_TRUE(ids.isEmpty());
    EXPECT_TRUE(albumsOf({}, FolderRoles(u"/Music/Tracks"_s, u"/Music/Albums"_s), &ids).isEmpty());
    EXPECT_TRUE(ids.isEmpty());
}

// --- Albums by folder role (albumsOf) ----------------------------------------------------------------------------

// The tracks and albums folders of the tests below.
FolderRoles roles()
{
    return {u"/Music/tracks"_s, u"/Music/albums"_s};
}

// The maintainer's "4 albums in ~/Music" (beta feedback): loose tracks, several from the same releases, and an empty
// albums folder. Guessing from tags made albums out of the loose tracks; the folder roles make none.
QList<ScannedFile> looseTracks()
{
    QList<ScannedFile> files;
    for (int i = 1; i <= 3; ++i) {
        files.append(file(u"/Music/tracks/Beau Didier - Take %1 [BEAU009].mp3"_s.arg(i), u"Beau Didier"_s,
                          u"Take %1"_s.arg(i), u"Take EP"_s, i, u"Beau Didier"_s, 2023));
        files.append(file(u"/Music/tracks/Bicep - Isles %1.mp3"_s.arg(i), u"Bicep"_s, u"Song %1"_s.arg(i), u"Isles"_s,
                          i, u"Bicep"_s, 2021));
    }
    files.append(file(u"/Music/tracks/Aphex Twin - Flaphead.mp3"_s, u"Aphex Twin"_s, u"Flaphead"_s));
    return files;
}

TEST(Albums, LooseTracksAndAnEmptyAlbumsFolderMakeNoAlbum)
{
    const QList<ScannedFile> files = looseTracks();
    EXPECT_FALSE(groupAlbums(files, nullptr).isEmpty()); // the guess that counted albums in a folder of loose tracks
    QHash<QString, quint64> ids;
    EXPECT_TRUE(albumsOf(files, roles(), &ids).isEmpty());
    EXPECT_TRUE(ids.isEmpty());
}

TEST(Albums, EachSubfolderOfTheAlbumsFolderIsOneAlbum)
{
    QList<ScannedFile> files = looseTracks() + islesFolder(u"/Music/albums/Bicep - Isles"_s);
    // Two untagged, unnumbered files are an album too: the folder says so, whatever the tags.
    files.append(file(u"/Music/albums/Live at Home/Intro.mp3"_s, u"Okuma Drift"_s, u"Intro"_s));
    files.append(file(u"/Music/albums/Live at Home/Outro.mp3"_s, u"Okuma Drift"_s, u"Outro"_s));
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = albumsOf(files, roles(), &ids);
    ASSERT_EQ(albums.size(), 2);
    EXPECT_EQ(albums.at(0).title, u"Isles"_s); // sorted by folder: "Bicep - Isles", "Live at Home"
    EXPECT_EQ(albums.at(0).artist, u"Bicep"_s);
    EXPECT_EQ(albums.at(0).year, 2021);
    EXPECT_EQ(albums.at(0).source, selecta::match::Source::Collection);
    EXPECT_EQ(albums.at(1).title, u"Live at Home"_s); // no album tag, nothing in the name: the folder's name
    EXPECT_EQ(albums.at(1).artist, u"Okuma Drift"_s);
    EXPECT_EQ(ids.size(), 5); // the loose tracks are in no album
    EXPECT_EQ(ids.value(u"/Music/albums/Bicep - Isles/01 Atlas.mp3"_s), albums.at(0).id);
    EXPECT_EQ(ids.value(u"/Music/albums/Live at Home/Outro.mp3"_s), albums.at(1).id);
    EXPECT_FALSE(ids.contains(u"/Music/tracks/Bicep - Isles 1.mp3"_s));
}

TEST(Albums, TagsNeverSplitAnAlbumFolder)
{
    // Another album's track and an untagged one in the folder: still one album, with every file in it.
    QList<ScannedFile> files = islesFolder(u"/Music/albums/Isles"_s);
    files.append(file(u"/Music/albums/Isles/Glue.mp3"_s, u"Bicep"_s, u"Glue"_s, u"Bicep"_s));
    files.append(file(u"/Music/albums/Isles/Untagged.mp3"_s, u"Bicep"_s, u"Untagged"_s));
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = albumsOf(files, roles(), &ids);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(albums.front().title, u"Isles"_s); // the most common album tag
    EXPECT_EQ(ids.size(), 5);
}

TEST(Albums, EverythingUnderAnAlbumFolderBelongsToIt)
{
    QList<ScannedFile> files;
    for (const QString &disc : {u"CD1"_s, u"Disc 2"_s}) {
        files.append(file(u"/Music/albums/Moderat - II/%1/01 Track.mp3"_s.arg(disc), u"Moderat"_s, disc, u"II"_s, 1));
    }
    // An artist folder holding two albums is one album: each first-level folder is one (the maintainer's rule).
    files.append(file(u"/Music/albums/Bicep/Isles/01 Atlas.mp3"_s, u"Bicep"_s, u"Atlas"_s, u"Isles"_s, 1));
    files.append(file(u"/Music/albums/Bicep/Bicep/01 Glue.mp3"_s, u"Bicep"_s, u"Glue"_s, u"Bicep"_s, 1));
    QHash<QString, quint64> ids;
    const QList<LibraryAlbum> albums = albumsOf(files, roles(), &ids);
    ASSERT_EQ(albums.size(), 2);
    EXPECT_EQ(albums.at(0).artist, u"Bicep"_s);
    EXPECT_EQ(albums.at(1).title, u"II"_s);
    EXPECT_EQ(ids.value(u"/Music/albums/Bicep/Isles/01 Atlas.mp3"_s),
              ids.value(u"/Music/albums/Bicep/Bicep/01 Glue.mp3"_s));
    EXPECT_EQ(ids.size(), 4);
}

TEST(Albums, LooseFilesInTheAlbumsFolderAreInNoAlbum)
{
    const QList<ScannedFile> files = islesFolder(u"/Music/albums"_s); // three tagged files lying in the albums folder
    QHash<QString, quint64> ids;
    EXPECT_TRUE(albumsOf(files, roles(), &ids).isEmpty());
    EXPECT_TRUE(ids.isEmpty());
}

TEST(Albums, AnAlbumFolderNamesWhatItsTagsDont)
{
    const QList<ScannedFile> files{file(u"/Music/albums/Bicep - Isles (2021)/Atlas.flac"_s, {}, u"Atlas"_s),
                                   file(u"/Music/albums/Bicep - Isles (2021)/Saku.flac"_s, {}, u"Saku"_s)};
    const QList<LibraryAlbum> albums = albumsOf(files, roles(), nullptr);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_EQ(albums.front().title, u"Isles"_s);
    EXPECT_EQ(albums.front().artist, u"Bicep"_s);
    EXPECT_EQ(albums.front().year, 2021);
}

TEST(Albums, CompilationsInTheAlbumsFolderAreMarked)
{
    QList<ScannedFile> files;
    const QStringList artists{u"Bicep"_s, u"Moderat"_s, u"Daft Punk"_s, u"Bonobo"_s};
    for (qsizetype i = 0; i < artists.size(); ++i) {
        files.append(file(u"/Music/albums/Summer/%1.mp3"_s.arg(i), artists.at(i), u"Song %1"_s.arg(i), u"Summer"_s,
                          static_cast<int>(i) + 1));
    }
    const QList<LibraryAlbum> albums = albumsOf(files, roles(), nullptr);
    ASSERT_EQ(albums.size(), 1);
    EXPECT_TRUE(albums.front().variousArtists);
    EXPECT_EQ(albums.front().artist, u"Various Artists"_s);
}

TEST(Albums, AlbumFolderIdsAreStable)
{
    const QList<ScannedFile> isles = islesFolder(u"/Music/albums/Isles"_s);
    const quint64 alone = albumsOf(isles, roles(), nullptr).constFirst().id;
    const QList<ScannedFile> both = islesFolder(u"/Music/albums/Deluxe"_s) + isles;
    const QList<LibraryAlbum> albums = albumsOf(both, roles(), nullptr);
    ASSERT_EQ(albums.size(), 2);
    EXPECT_NE(albums.at(0).id, albums.at(1).id);
    EXPECT_EQ(albums.at(1).id, alone); // "Isles" sorts after "Deluxe"; the same folder, the same id
    // The id doesn't depend on how the folder's name is normalised (macOS may report NFD).
    const QList<ScannedFile> nfc = islesFolder(u"/Music/albums/Bj\u00F6rk"_s);
    const QList<ScannedFile> nfd = islesFolder(u"/Music/albums/Bjo\u0308rk"_s);
    EXPECT_EQ(albumsOf(nfc, roles(), nullptr).constFirst().id, albumsOf(nfd, roles(), nullptr).constFirst().id);
}

TEST(Albums, NoAlbumsFolderNoAlbums)
{
    const QList<ScannedFile> files = islesFolder(u"/Music/albums/Isles"_s);
    EXPECT_TRUE(albumsOf(files, FolderRoles(u"/Music/tracks"_s, {}), nullptr).isEmpty());
    EXPECT_TRUE(albumsOf(files, FolderRoles(), nullptr).isEmpty());
}

} // namespace
