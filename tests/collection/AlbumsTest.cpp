// SPDX-License-Identifier: GPL-3.0-or-later
// Album folders among scanned files (note §8 step 1): tags, folder names, disc folders, various artists, stable ids.
#include "collection/Albums.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
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
}

} // namespace
