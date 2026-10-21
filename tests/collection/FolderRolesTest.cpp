// SPDX-License-Identifier: GPL-3.0-or-later
// The collection's folder roles (PRD LIB-1): loose tracks in the tracks folder, one album per subfolder of the albums
// folder.
#include "collection/FolderRoles.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::collection::FolderRoles;
using Role = selecta::collection::FolderRoles::Role;

namespace {

// The album folder of `path`, or an empty string when it isn't in an album.
QString albumOf(const FolderRoles &roles, const QString &path)
{
    QString folder;
    return roles.roleOf(path, &folder) == Role::Album ? folder : QString();
}

TEST(FolderRoles, TheTracksFolderHoldsItsLooseFilesOnly)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks/Bicep - Glue.mp3"_s), Role::Track);
    // Subfolders are left out, and nothing in the tracks folder is ever an album.
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks/Isles/01 Atlas.mp3"_s), Role::None);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks/A/B/C.mp3"_s), Role::None);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks"_s), Role::None); // the folder itself
}

TEST(FolderRoles, EachSubfolderOfTheAlbumsFolderIsOneAlbum)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Albums/Bicep - Isles/01 Atlas.mp3"_s), Role::Album);
    EXPECT_EQ(albumOf(roles, u"/Music/Albums/Bicep - Isles/01 Atlas.mp3"_s), u"/Music/Albums/Bicep - Isles"_s);
    // Disc folders, and anything deeper, belong to the album of the first-level folder.
    EXPECT_EQ(albumOf(roles, u"/Music/Albums/Moderat - II/CD2/01 Track.mp3"_s), u"/Music/Albums/Moderat - II"_s);
    EXPECT_EQ(albumOf(roles, u"/Music/Albums/Bicep/Isles/01 Atlas.mp3"_s), u"/Music/Albums/Bicep"_s);
    // Files lying in the albums folder itself aren't part of the collection.
    EXPECT_EQ(roles.roleOf(u"/Music/Albums/Loose.mp3"_s), Role::None);
    QString folder = u"untouched"_s;
    EXPECT_EQ(roles.roleOf(u"/Music/Albums/Loose.mp3"_s, &folder), Role::None);
    EXPECT_EQ(folder, u"untouched"_s);
}

TEST(FolderRoles, FilesOutsideBothFoldersAreNotInTheCollection)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Elsewhere.mp3"_s), Role::None);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks2/Song.mp3"_s), Role::None); // a name prefix isn't a parent folder
    EXPECT_EQ(roles.roleOf(u"/Music/AlbumsOld/X/Song.mp3"_s), Role::None);
}

TEST(FolderRoles, FoldersNotChosenHoldNothing)
{
    const FolderRoles none;
    EXPECT_TRUE(none.isEmpty());
    EXPECT_TRUE(none.roots().isEmpty());
    EXPECT_EQ(none.roleOf(u"/Music/Song.mp3"_s), Role::None);
    EXPECT_TRUE(none.rescanFor(u"/Music"_s).isEmpty());

    const FolderRoles tracksOnly(u"/Music/Tracks"_s, {});
    EXPECT_FALSE(tracksOnly.isEmpty());
    EXPECT_EQ(tracksOnly.roots(), QStringList{u"/Music/Tracks"_s});
    EXPECT_EQ(tracksOnly.roleOf(u"/Music/Tracks/Song.mp3"_s), Role::Track);
    EXPECT_EQ(tracksOnly.roleOf(u"/Music/Albums/X/Song.mp3"_s), Role::None);

    const FolderRoles albumsOnly({}, u"/Music/Albums"_s);
    EXPECT_EQ(albumsOnly.roots(), QStringList{u"/Music/Albums"_s});
    EXPECT_EQ(albumsOnly.roleOf(u"/Music/Albums/X/Song.mp3"_s), Role::Album);

    // A relative path isn't a folder anyone chose.
    const FolderRoles relative(u"Music/Tracks"_s, u"Albums"_s);
    EXPECT_TRUE(relative.isEmpty());
    EXPECT_TRUE(relative.tracks().isEmpty());
}

TEST(FolderRoles, PathsAreCleanedAndComparedAsNfc)
{
    const FolderRoles roles(u"/Music/./Tracks/"_s, u"/Music/Albums/../Albums"_s);
    EXPECT_EQ(roles.tracks(), u"/Music/Tracks"_s);
    EXPECT_EQ(roles.albums(), u"/Music/Albums"_s);
    EXPECT_EQ(roles, FolderRoles(u"/Music/Tracks"_s, u"/Music/Albums"_s));
    EXPECT_FALSE(roles == FolderRoles(u"/Music/Albums"_s, u"/Music/Tracks"_s)); // the roles matter, not just the set
    // macOS may report NFD names: "Björk" decomposed matches the folder chosen in NFC, and the album folder keeps the
    // spelling of the file's path.
    const QString nfc = u"/Music/Bj\u00F6rk Albums"_s;
    const QString nfd = u"/Music/Bjo\u0308rk Albums"_s;
    const FolderRoles decomposed({}, nfc);
    EXPECT_EQ(decomposed.roleOf(nfd + u"/Homogenic/01 Hunter.flac"_s), Role::Album);
    EXPECT_EQ(albumOf(decomposed, nfd + u"/Homogenic/01 Hunter.flac"_s), nfd + u"/Homogenic"_s);
}

TEST(FolderRoles, ScanOptionsCarryTheRoles)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    selecta::collection::ScanOptions options;
    options.tracksFolders = {u"/stale"_s};
    roles.applyTo(options);
    EXPECT_EQ(options.tracksFolders, QStringList{u"/Music/Tracks"_s});
    EXPECT_EQ(options.albumsFolders, QStringList{u"/Music/Albums"_s});
    FolderRoles().applyTo(options);
    EXPECT_TRUE(options.tracksFolders.isEmpty());
    EXPECT_TRUE(options.albumsFolders.isEmpty());
}

TEST(FolderRoles, RescansFollowTheRoles)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    EXPECT_EQ(roles.roots(), (QStringList{u"/Music/Tracks"_s, u"/Music/Albums"_s}));
    EXPECT_EQ(roles.rescanFor(u"/Music/Tracks"_s), QStringList{u"/Music/Tracks"_s});
    EXPECT_TRUE(roles.rescanFor(u"/Music/Tracks/Sub"_s).isEmpty()); // its subfolders concern nobody
    EXPECT_EQ(roles.rescanFor(u"/Music/Albums"_s), QStringList{u"/Music/Albums"_s});
    EXPECT_EQ(roles.rescanFor(u"/Music/Albums/Isles/CD1"_s), QStringList{u"/Music/Albums/Isles/CD1"_s});
    EXPECT_TRUE(roles.rescanFor(u"/Music"_s).isEmpty()); // above both: the Watcher reports the folders themselves
    EXPECT_TRUE(roles.rescanFor(u"/Elsewhere"_s).isEmpty());
}

// Settings keeps the folders apart; a layout that isn't (a hand-edited settings file) still scans sensibly.
TEST(FolderRoles, OneFolderForBothHoldsLooseTracksAndAlbumFolders)
{
    const FolderRoles roles(u"/Music"_s, u"/Music"_s);
    EXPECT_EQ(roles.roots(), QStringList{u"/Music"_s});
    EXPECT_EQ(roles.roleOf(u"/Music/Song.mp3"_s), Role::Track);
    EXPECT_EQ(albumOf(roles, u"/Music/Isles/01 Atlas.mp3"_s), u"/Music/Isles"_s);
    EXPECT_EQ(roles.rescanFor(u"/Music/Isles"_s), QStringList{u"/Music/Isles"_s});
}

TEST(FolderRoles, ATracksFolderInsideTheAlbumsFolderIsNeverAnAlbum)
{
    const FolderRoles roles(u"/Music/Tracks"_s, u"/Music"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks/Song.mp3"_s), Role::Track);
    EXPECT_EQ(roles.roleOf(u"/Music/Tracks/Sub/Song.mp3"_s), Role::None);
    EXPECT_EQ(albumOf(roles, u"/Music/Isles/01 Atlas.mp3"_s), u"/Music/Isles"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Loose.mp3"_s), Role::None);
    // A change above the tracks folder rescans it too: its role differs from its surroundings'.
    EXPECT_EQ(roles.rescanFor(u"/Music"_s), (QStringList{u"/Music"_s, u"/Music/Tracks"_s}));
    EXPECT_TRUE(roles.rescanFor(u"/Music/Tracks/Sub"_s).isEmpty());
}

TEST(FolderRoles, AnAlbumsFolderInsideTheTracksFolderStillHoldsAlbums)
{
    const FolderRoles roles(u"/Music"_s, u"/Music/Albums"_s);
    EXPECT_EQ(roles.roleOf(u"/Music/Song.mp3"_s), Role::Track);
    EXPECT_EQ(roles.roleOf(u"/Music/Other/Song.mp3"_s), Role::None);
    EXPECT_EQ(albumOf(roles, u"/Music/Albums/Isles/01 Atlas.mp3"_s), u"/Music/Albums/Isles"_s);
    // The tracks folder's own scan doesn't descend into the albums folder: a change there rescans both.
    EXPECT_EQ(roles.rescanFor(u"/Music"_s), (QStringList{u"/Music"_s, u"/Music/Albums"_s}));
    EXPECT_TRUE(roles.rescanFor(u"/Music/Other"_s).isEmpty());
    EXPECT_EQ(roles.rescanFor(u"/Music/Albums/Isles"_s), QStringList{u"/Music/Albums/Isles"_s});
}

TEST(FolderRoles, TheFileSystemRootWorksLikeAnyFolder)
{
    const FolderRoles roles({}, u"/"_s);
    EXPECT_EQ(albumOf(roles, u"/Isles/CD1/01 Atlas.mp3"_s), u"/Isles"_s);
    EXPECT_EQ(roles.roleOf(u"/Loose.mp3"_s), Role::None);
}

} // namespace
