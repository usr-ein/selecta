// SPDX-License-Identifier: GPL-3.0-or-later
// build() (docs/contracts/share.md): what the walk shares and skips, aliases, roots, attributes, and the share list as
// slsk's own decoder reads it.
#include "ShareTestSupport.h"
#include "SyntheticAudio.h"
#include "share/ShareIndex.h"
#include "share/detail/Attributes.h"
#include "share/detail/IndexBuilder.h"
#include "share/detail/Walk.h"
#include "slsk/PeerMessages.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_DARWIN
#include <sys/resource.h>
#endif

using namespace Qt::StringLiterals;
using selecta::share::build;
using selecta::share::BuildOptions;
using selecta::share::BuildResult;
using selecta::share::Layout;
using selecta::share::ShareIndex;
using selecta::share::ShareRoot;
using selecta::share::test::buildTree;
using selecta::share::test::decodedList;
using selecta::share::test::listedEntry;
using selecta::share::test::listedFiles;
using selecta::share::test::listedFolders;
using selecta::share::test::scanned;
using selecta::share::test::Tree;
using selecta::share::test::unreadablePaths;
namespace collection = selecta::collection;
namespace detail = selecta::share::detail;
namespace slsk = selecta::slsk;
namespace test = selecta::share::test;

namespace {

// Restores a folder's permissions when a test ends, so the temporary folder can be removed.
struct ModeGuard
{
    explicit ModeGuard(QString folder)
        : path(std::move(folder))
    {}
    ~ModeGuard() { test::setMode(path, 0755); }
    ModeGuard(const ModeGuard &) = delete;
    ModeGuard &operator=(const ModeGuard &) = delete;
    ModeGuard(ModeGuard &&) = delete;
    ModeGuard &operator=(ModeGuard &&) = delete;

    QString path;
};

// --- What is shared --------------------------------------------------------------------------------------------------

// The collection's two folders (PRD LIB-1, SHARE-1): peers get <tracks folder>\<track> and
// <albums folder>\<album>\<track>, nothing else.

TEST(ShareBuild, TheTracksFolderSharesTheTracksLyingInItAndNothingInItsSubfolders)
{
    Tree tree;
    tree.file(u"01 Loose.mp3"_s);
    tree.file(u"02 Loose.flac"_s);
    tree.file(u"Old Rips/03 Kept Aside.mp3"_s);
    tree.file(u"Old Rips/Deeper/04 Further.mp3"_s);
    tree.file(u"Some Album/05 Track.mp3"_s);
    const BuildResult result = buildTree(tree, {}, {}, Layout::Tracks);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\01 Loose.mp3"_s, u"Music\\02 Loose.flac"_s}));
    EXPECT_EQ(listedFolders(*result.index), QStringList({u"Music"_s}));
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{1, 2}));
    EXPECT_TRUE(result.unreadable.isEmpty());
    // Nothing below the tracks folder answers a search, a folder listing or an upload request.
    EXPECT_TRUE(result.index->search(u"Kept Aside"_s, {}).isEmpty());
    EXPECT_TRUE(result.index->search(u"Track"_s, {}).isEmpty());
    EXPECT_TRUE(result.index->folderContents(slsk::RemotePath::fromText(u"Music\\Old Rips"_s)).isEmpty());
    EXPECT_FALSE(result.index->lookup(slsk::RemotePath::fromText(u"Music\\Old Rips\\03 Kept Aside.mp3"_s)));
    EXPECT_TRUE(result.index->lookup(slsk::RemotePath::fromText(u"Music\\01 Loose.mp3"_s)));
}

TEST(ShareBuild, TheAlbumsFolderSharesTheTracksOfEachAlbumAndNothingLooseOrDeeper)
{
    Tree tree;
    tree.file(u"Loose On Top.mp3"_s);
    tree.file(u"Kel Varo - Amber Engine/01 Amber Engine.flac"_s);
    tree.file(u"Kel Varo - Amber Engine/02 Night Ferry.flac"_s);
    tree.file(u"Kel Varo - Amber Engine/Disc 2/01 Bonus.flac"_s);
    tree.file(u"Kel Varo - Amber Engine/Scans/Hidden Track.mp3"_s);
    tree.file(u"Okuma Drift - Neon Rain/01 Neon Rain.mp3"_s);
    tree.folder(u"Empty Album"_s);
    tree.file(u"Only Subfolders/CD1/01.mp3"_s);
    const BuildResult result = buildTree(tree, {}, {}, Layout::Albums);
    EXPECT_EQ(listedFiles(*result.index),
              QStringList({u"Music\\Kel Varo - Amber Engine\\01 Amber Engine.flac"_s,
                           u"Music\\Kel Varo - Amber Engine\\02 Night Ferry.flac"_s,
                           u"Music\\Okuma Drift - Neon Rain\\01 Neon Rain.mp3"_s}));
    EXPECT_EQ(listedFolders(*result.index),
              QStringList({u"Music"_s, u"Music\\Kel Varo - Amber Engine"_s, u"Music\\Okuma Drift - Neon Rain"_s}));
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{3, 3}));
    EXPECT_TRUE(result.unreadable.isEmpty());
    EXPECT_TRUE(result.index->search(u"Loose"_s, {}).isEmpty());
    EXPECT_TRUE(result.index->search(u"Bonus"_s, {}).isEmpty());
    EXPECT_TRUE(result.index->folderContents(slsk::RemotePath::fromText(u"Music\\Kel Varo - Amber Engine\\Disc 2"_s))
                    .isEmpty());
    EXPECT_FALSE(result.index->lookup(slsk::RemotePath::fromText(u"Music\\Loose On Top.mp3"_s)));
    EXPECT_FALSE(
        result.index->lookup(slsk::RemotePath::fromText(u"Music\\Kel Varo - Amber Engine\\Disc 2\\01 Bonus.flac"_s)));
}

TEST(ShareBuild, AFolderInBothRolesSharesItsLooseTracksAndItsAlbums)
{
    Tree tree;
    tree.file(u"Loose.mp3"_s);
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Album/CD2/02.mp3"_s);
    const BuildResult result = buildTree(tree, {}, {}, Layout::TracksAndAlbums);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Loose.mp3"_s, u"Music\\Album\\01.mp3"_s}));
}

TEST(ShareBuild, OnlyTracksAreSharedInBothFolders)
{
    Tree tracks;
    Tree albums;
    const QStringList others{u"cover.jpg"_s, u"back.jpeg"_s, u"cd.png"_s,      u"album.cue"_s, u"rip.log"_s,
                             u"info.nfo"_s,  u"notes.txt"_s, u"booklet.pdf"_s, u"setup.exe"_s, u"list.m3u"_s,
                             u"list.m3u8"_s, u"archive.zip"_s, u"anim.gif"_s,  u"README"_s,    u".DS_Store"_s};
    for (const QString &name : others) {
        tracks.file(name);
        albums.file(u"Album/"_s + name);
    }
    for (const QString &name : {u"01.mp3"_s, u"02.FLAC"_s, u"03.wav"_s, u"04.aiff"_s, u"05.m4a"_s, u"06.ogg"_s,
                                u"07.opus"_s}) {
        tracks.file(name);
        albums.file(u"Album/"_s + name);
    }
    const BuildResult result =
        build({{tracks.root(), u"Tracks"_s, Layout::Tracks}, {albums.root(), u"Albums"_s, Layout::Albums}}, {});
    EXPECT_TRUE(result.unreadable.isEmpty());
    QStringList expected;
    for (const QString &name : {u"01.mp3"_s, u"02.FLAC"_s, u"03.wav"_s, u"04.aiff"_s, u"05.m4a"_s, u"06.ogg"_s,
                                u"07.opus"_s}) {
        expected.append(u"Tracks\\"_s + name);
    }
    for (const QString &name : {u"01.mp3"_s, u"02.FLAC"_s, u"03.wav"_s, u"04.aiff"_s, u"05.m4a"_s, u"06.ogg"_s,
                                u"07.opus"_s}) {
        expected.append(u"Albums\\Album\\"_s + name);
    }
    EXPECT_EQ(listedFiles(*result.index), expected);
    EXPECT_TRUE(result.index->search(u"cover"_s, {}).isEmpty());
    EXPECT_FALSE(result.index->lookup(slsk::RemotePath::fromText(u"Albums\\Album\\cover.jpg"_s)));
}

TEST(ShareBuild, AFolderOfCoversAloneIsNoAlbum)
{
    Tree tree;
    tree.file(u"Artwork/cover.jpg"_s);
    tree.file(u"Album/01.mp3"_s);
    EXPECT_EQ(listedFolders(*buildTree(tree).index), QStringList({u"Music"_s, u"Music\\Album"_s}));
}

TEST(ShareBuild, WhatALayoutLeavesOutIsNeitherListedNorReported)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permissions don't apply to root";
    }
    // A folder the layout never walks is never listed, so it can't be reported as unreadable; names that couldn't be
    // shared are only reported where tracks are shared.
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Album/Locked/02.mp3"_s);
    const ModeGuard guard{tree.path(u"Album/Locked"_s)};
    ASSERT_TRUE(test::setMode(guard.path, 0));
    tree.nativeFile("line\nbreak.mp3");
    tree.file(u"Album/Bad\tSub/03.mp3"_s);
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(ShareBuild, SkipsHiddenAppleDoubleAndTemporaryFiles)
{
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Album/.secret.mp3"_s);
    tree.file(u"Album/._01.mp3"_s);
    tree.file(u"Album/.DS_Store"_s);
    tree.file(u".Hidden Folder/02.mp3"_s);
    tree.file(u"Album/03.mp3.selecta-part"_s);
    tree.file(u"Album/04.mp3.part"_s);
    tree.file(u"Album/~$05.mp3"_s);
    tree.file(u"Album/06.mp3~"_s);
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(ShareBuild, SkipsWhatTheHiddenFlagHides)
{
#ifndef UF_HIDDEN
    GTEST_SKIP() << "no hidden flag on this platform (macOS's chflags hidden)";
#else
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    ASSERT_TRUE(test::setHidden(tree.file(u"Album/02.mp3"_s)));
    tree.file(u"Secret/03.mp3"_s);
    ASSERT_TRUE(test::setHidden(tree.path(u"Secret"_s)));
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
#endif
}

TEST(ShareBuild, SkipsEmptyFiles)
{
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Album/empty.mp3"_s, QByteArray());
    tree.file(u"Album/empty.txt"_s, QByteArray());
    EXPECT_EQ(listedFiles(*buildTree(tree).index), QStringList({u"Music\\Album\\01.mp3"_s}));
}

TEST(ShareBuild, SkipsSystemJunkFolders)
{
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"$RECYCLE.BIN/S-1-5/$R1234.mp3"_s);
    tree.file(u"System Volume Information/x.mp3"_s);
    tree.file(u"Album/@eaDir/01.mp3/SYNOPHOTO_THUMB.jpg"_s);
    tree.file(u"#recycle/old.mp3"_s);
    tree.file(u"#snapshot/old.mp3"_s);
    tree.file(u"Network Trash Folder/old.mp3"_s);
    tree.file(u"Temporary Items/old.mp3"_s);
    EXPECT_EQ(listedFiles(*buildTree(tree).index), QStringList({u"Music\\Album\\01.mp3"_s}));
}

TEST(ShareBuild, NeverSharesExcludedFolders)
{
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Selecta Inbox/new.mp3"_s);
    tree.file(u"Cache/preview.mp3"_s);
    tree.file(u"Staging/sub/x.flac"_s);
    Tree elsewhere;
    // The cache is named through a symlink, the staging folder with a trailing slash and a detour: the same folders.
    ASSERT_TRUE(elsewhere.symlink(tree.path(u"Cache"_s), u"cache-link"_s));
    BuildOptions options;
    options.excludedFolders = {tree.path(u"Selecta Inbox"_s), elsewhere.path(u"cache-link"_s),
                               tree.path(u"Album/../Staging/"_s), QString(), tree.path(u"Not There"_s)};
    const BuildResult result = buildTree(tree, {}, options);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(ShareBuild, ARootInsideAnExcludedFolderSharesNothing)
{
    Tree tree;
    tree.file(u"Inbox/Album/01.mp3"_s);
    BuildOptions options;
    options.excludedFolders = {tree.path(u"Inbox"_s)};
    const BuildResult result = build({{tree.path(u"Inbox/Album"_s), u"Album"_s}}, {}, options);
    EXPECT_TRUE(listedFiles(*result.index).isEmpty());
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{0, 0}));
}

// --- Symlinks, hard links, overlapping roots -------------------------------------------------------------------------

TEST(ShareBuild, NeverFollowsSymlinks)
{
    Tree tree;
    Tree outside;
    tree.file(u"Album/01.mp3"_s);
    outside.file(u"Private/secret.mp3"_s);
    ASSERT_TRUE(tree.symlink(outside.path(u"Private/secret.mp3"_s), u"Album/linked-out.mp3"_s));
    ASSERT_TRUE(tree.symlink(outside.path(u"Private"_s), u"Private Link"_s));
    ASSERT_TRUE(tree.symlink(tree.path(u"Album/01.mp3"_s), u"Album/linked-in.mp3"_s));
    ASSERT_TRUE(tree.symlink(tree.path(u"Album"_s), u"Album Link"_s));
    ASSERT_TRUE(tree.symlink(tree.root(), u"Album/loop"_s));
    ASSERT_TRUE(tree.symlink(u"dangling-target.mp3"_s, u"Album/dangling.mp3"_s));
    ASSERT_TRUE(tree.symlink(tree.path(u"Album/ping"_s), u"Album/pong"_s));
    ASSERT_TRUE(tree.symlink(tree.path(u"Album/pong"_s), u"Album/ping"_s));
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(ShareBuild, ARootThatIsASymlinkIsFollowed)
{
    Tree real;
    real.file(u"Album/01.mp3"_s);
    Tree links;
    ASSERT_TRUE(links.symlink(real.root(), u"Music Link"_s));
    const BuildResult result = build({{links.path(u"Music Link"_s), u"Music"_s, Layout::Albums}}, {});
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
}

TEST(ShareBuild, SharesAHardLinkedFileOnce)
{
    Tree tree;
    const QString first = tree.file(u"A/01.mp3"_s);
    tree.folder(u"B"_s);
    ASSERT_EQ(::link(QFile::encodeName(first).constData(), QFile::encodeName(tree.path(u"B/copy.mp3"_s)).constData()),
              0);
    EXPECT_EQ(listedFiles(*buildTree(tree).index), QStringList({u"Music\\A\\01.mp3"_s}));
}

TEST(ShareBuild, ARootInsideAnotherIsSharedAsItselfOnly)
{
    // The tracks folder inside the albums folder (core refuses that layout, the build stays sensible): its tracks are
    // loose tracks, never an album's, whatever the order of the roots.
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    tree.file(u"Singles/02.mp3"_s);
    const ShareRoot albums{tree.root(), u"Albums"_s, Layout::Albums};
    const ShareRoot tracks{tree.path(u"Singles"_s), u"Tracks"_s, Layout::Tracks};
    const QStringList expected{u"Albums\\Album\\01.mp3"_s, u"Tracks\\02.mp3"_s};
    QStringList outerFirst = listedFiles(*build({albums, tracks}, {}).index);
    QStringList innerFirst = listedFiles(*build({tracks, albums}, {}).index);
    outerFirst.sort();
    innerFirst.sort();
    EXPECT_EQ(outerFirst, expected);
    EXPECT_EQ(innerFirst, expected);
    // The same folder twice: shared once, under the first.
    const BuildResult twice = build({albums, {tree.root(), u"Again"_s, Layout::Albums}}, {});
    EXPECT_EQ(twice.index->counts(), (ShareIndex::Counts{3, 2}));
}

// --- Roots and aliases -----------------------------------------------------------------------------------------------

TEST(ShareBuild, DuplicateAliasesAreMadeUnique)
{
    Tree first;
    Tree second;
    Tree third;
    first.file(u"a.mp3"_s);
    second.file(u"b.mp3"_s);
    third.file(u"c.mp3"_s);
    const BuildResult result =
        build({{first.root(), u"Music"_s}, {second.root(), u"Music"_s}, {third.root(), u"music"_s}}, {});
    EXPECT_EQ(listedFiles(*result.index),
              QStringList({u"Music\\a.mp3"_s, u"Music (2)\\b.mp3"_s, u"music (3)\\c.mp3"_s}));
    const QList<ShareRoot> roots = result.index->roots();
    ASSERT_EQ(roots.size(), 3);
    EXPECT_EQ(roots.at(1).alias, u"Music (2)"_s);
    EXPECT_EQ(roots.at(1).path, QDir::cleanPath(second.root()));
}

TEST(ShareBuild, AliasesAreCleaned)
{
    Tree tree;
    tree.file(u"a.mp3"_s);
    EXPECT_EQ(listedFiles(*build({{tree.root(), u"My\\Music/Here"_s}}, {}).index),
              QStringList({u"My_Music_Here\\a.mp3"_s}));
    EXPECT_EQ(listedFiles(*build({{tree.root(), QString()}}, {}).index), QStringList({u"Shared\\a.mp3"_s}));
}

TEST(ShareBuild, AMissingRootIsReportedKeepsItsAliasAndSharesNothing)
{
    Tree tree;
    tree.file(u"a.mp3"_s);
    const QString missing = tree.path(u"Unplugged Drive"_s);
    const BuildResult result = build({{missing, u"Music"_s}, {tree.root(), u"Music"_s}}, {});
    EXPECT_EQ(unreadablePaths(result), QStringList({missing}));
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music (2)\\a.mp3"_s}));
    EXPECT_EQ(result.index->roots().at(0), (ShareRoot{missing, u"Music"_s}));
}

TEST(ShareBuild, RootsThatArentFoldersOrArentAbsoluteAreReported)
{
    Tree tree;
    const QString file = tree.file(u"a.mp3"_s);
    const BuildResult result =
        build({{file, u"File"_s}, {u"relative/Music"_s, u"Relative"_s}, {QString(), u"E"_s}}, {});
    EXPECT_EQ(unreadablePaths(result), QStringList({u"relative/Music"_s, QString(), file}));
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{0, 0}));
    EXPECT_FALSE(result.cancelled);
}

TEST(ShareBuild, AFolderThatCantBeListedIsReportedAndTheRestShared)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permissions don't apply to root";
    }
    Tree tree;
    tree.file(u"Open/01.mp3"_s);
    tree.file(u"Locked/02.mp3"_s);
    const ModeGuard guard{tree.path(u"Locked"_s)};
    ASSERT_TRUE(test::setMode(guard.path, 0));
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Open\\01.mp3"_s}));
    EXPECT_EQ(unreadablePaths(result), QStringList({guard.path}));
}

TEST(ShareBuild, EntriesWhoseAttributesCantBeReadAreReported)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permissions don't apply to root";
    }
    // Readable but not searchable: the folder lists, its entries can't be stat'ed.
    Tree tree;
    tree.file(u"Open/01.mp3"_s);
    const QString file = tree.file(u"NoSearch/02.mp3"_s);
    const ModeGuard guard{tree.path(u"NoSearch"_s)};
    ASSERT_TRUE(test::setMode(guard.path, 0444));
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Open\\01.mp3"_s}));
    EXPECT_EQ(unreadablePaths(result), QStringList({file}));
}

TEST(ShareBuild, TheThreadsMaterialisationPolicyIsRestored)
{
#ifdef Q_OS_DARWIN
    const int before = ::getiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD);
    ASSERT_GE(before, 0);
    if (::setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD,
                         IOPOL_MATERIALIZE_DATALESS_FILES_ON) != 0) {
        GTEST_SKIP() << "this thread's materialisation policy can't be changed";
    }
    Tree tree;
    tree.file(u"a.mp3"_s, test::mp3Cbr320());
    EXPECT_EQ(buildTree(tree, {}, {}, Layout::Tracks).index->counts().files, 1);
    EXPECT_EQ(::getiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD),
              IOPOL_MATERIALIZE_DATALESS_FILES_ON);
    (void)::setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, before);
#else
    GTEST_SKIP() << "iCloud materialisation is macOS only";
#endif
}

// --- The tree peers see ----------------------------------------------------------------------------------------------

TEST(ShareBuild, LeavesEmptyFoldersOutAndKeepsParents)
{
    Tree tree;
    tree.file(u"A/x.mp3"_s);
    tree.folder(u"Empty"_s);
    tree.file(u"Unshared/setup.exe"_s);
    tree.file(u"Hidden Only/.x.mp3"_s);
    const BuildResult result = buildTree(tree);
    // The albums folder holds no track of its own: it stays, as the parent of its albums.
    EXPECT_EQ(listedFolders(*result.index), QStringList({u"Music"_s, u"Music\\A"_s}));
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{2, 1}));
}

TEST(ShareBuild, WalksFoldersBeforeTheirSubfoldersInNameOrder)
{
    Tree tree;
    tree.file(u"b.mp3"_s);
    tree.file(u"B/1.mp3"_s);
    tree.file(u"A/2.mp3"_s);
    tree.file(u"A/1.mp3"_s);
    tree.file(u"a.mp3"_s);
    const BuildResult result = buildTree(tree, {}, {}, Layout::TracksAndAlbums);
    EXPECT_EQ(listedFolders(*result.index), QStringList({u"Music"_s, u"Music\\A"_s, u"Music\\B"_s}));
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\a.mp3"_s, u"Music\\b.mp3"_s, u"Music\\A\\1.mp3"_s,
                                                       u"Music\\A\\2.mp3"_s, u"Music\\B\\1.mp3"_s}));
}

TEST(ShareBuild, NfdNamesAreSharedInNfc)
{
    Tree tree;
    ASSERT_TRUE(QDir().mkpath(tree.path(u"Album"_s)));
    ASSERT_FALSE(tree.nativeFile(u"Album/Cafe\u0301 Del Mar.mp3"_s.toUtf8()).isEmpty()); // NFD on disk
    ASSERT_TRUE(QDir().mkpath(QFile::decodeName(QFile::encodeName(tree.root()) + "/Ro\xcc\x88yksopp")));
    ASSERT_FALSE(tree.nativeFile(QByteArray("Ro\xcc\x88yksopp/01.mp3")).isEmpty());
    const QStringList files = listedFiles(*buildTree(tree).index);
    EXPECT_EQ(files, QStringList({u"Music\\Album\\Caf\u00e9 Del Mar.mp3"_s, u"Music\\R\u00f6yksopp\\01.mp3"_s}));
    for (const QString &file : files) {
        EXPECT_EQ(file, file.normalized(QString::NormalizationForm_C));
    }
}

TEST(ShareBuild, ABackslashInANameIsReplacedForPeers)
{
    Tree tree;
    tree.file(u"AC\\DC/Back\\In Black.mp3"_s);
    EXPECT_EQ(listedFiles(*buildTree(tree).index), QStringList({u"Music\\AC＼DC\\Back＼In Black.mp3"_s}));
}

TEST(ShareBuild, NamesThatCantBeSharedAreReported)
{
    Tree tree;
    tree.file(u"Album/01.mp3"_s);
    const QByteArray controlName = tree.nativeFile("Album/line\nbreak.mp3");
    ASSERT_TRUE(QDir().mkpath(tree.path(u"Bad\tFolder"_s)));
    tree.file(u"Bad\tFolder/02.mp3"_s);
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\Album\\01.mp3"_s}));
    EXPECT_EQ(result.unreadable.size(), 2);
    EXPECT_TRUE(unreadablePaths(result).contains(QFile::decodeName(controlName)));
}

// --- Limits, placeholders and cancellation (the walk itself)
// ----------------------------------------------------------

detail::Walk walkTree(const Tree &tree, const detail::WalkOptions &options,
                      const selecta::audio::CancellationFlag *cancel = nullptr)
{
    const QHash<QString, collection::ScannedFile> none;
    detail::AttributeSource attributes(none);
    return detail::walk({{tree.root(), u"Music"_s, Layout::TracksAndAlbums}}, {}, attributes, cancel, options);
}

TEST(ShareBuild, PathsTooLongForPeersAreReported)
{
    // build() keeps virtual paths within 4 KB, which peers' requests can reach; macOS' PATH_MAX of 1 KB stops local
    // paths first, so the limit is exercised small.
    Tree tree;
    tree.file(u"ok.mp3"_s);
    tree.file(u"A Long Folder Name/inside.mp3"_s);
    tree.file(u"a-very-long-file-name.mp3"_s);
    detail::WalkOptions options;
    options.maxVirtualPathBytes = 20; // "Music\\ok.mp3" fits
    const detail::Walk walk = walkTree(tree, options);
    EXPECT_EQ(walk.files, 1);
    ASSERT_EQ(walk.unreadable.size(), 2);
    EXPECT_TRUE(walk.unreadable.at(0).first.endsWith(u"/A Long Folder Name"_s));
    EXPECT_TRUE(walk.unreadable.at(1).first.endsWith(u"/a-very-long-file-name.mp3"_s));
    for (const auto &[path, reason] : walk.unreadable) {
        EXPECT_TRUE(reason.contains(u"too long"_s));
    }
}

TEST(ShareBuild, TheWalkStopsAtTheFileLimit)
{
    Tree tree;
    for (int i = 0; i < 6; ++i) {
        tree.file(u"%1.mp3"_s.arg(i));
    }
    detail::WalkOptions options;
    options.maxFiles = 4;
    const detail::Walk walk = walkTree(tree, options);
    EXPECT_EQ(walk.files, 4);
    ASSERT_EQ(walk.unreadable.size(), 1);
    EXPECT_EQ(walk.unreadable.at(0).first, tree.root());
    EXPECT_FALSE(walk.cancelled);
}

TEST(ShareBuild, TheWalkStopsAtTheStringLimit)
{
    Tree tree;
    for (int i = 0; i < 6; ++i) {
        tree.file(u"%1.mp3"_s.arg(i));
    }
    detail::WalkOptions options;
    options.maxPoolBytes = 1;
    const detail::Walk walk = walkTree(tree, options);
    EXPECT_EQ(walk.files, 0);
    EXPECT_EQ(walk.unreadable.size(), 1);
}

TEST(ShareBuild, ICloudPlaceholdersAreReportedAndNeverShared)
{
#ifndef SF_DATALESS
    GTEST_SKIP() << "no dataless files (iCloud placeholders) on this platform";
#else
    Tree tree;
    tree.file(u"a.mp3"_s);
    tree.file(u"b.mp3"_s, test::mp3Cbr320());
    detail::WalkOptions options;
    options.afterStat = [](const QByteArray &path, struct stat &status) {
        if (path.endsWith("/b.mp3")) {
            status.st_flags |= SF_DATALESS;
        }
    };
    const detail::Walk walk = walkTree(tree, options);
    ASSERT_EQ(walk.folders.size(), 1U);
    ASSERT_EQ(walk.folders.at(0).files.size(), 1U);
    EXPECT_EQ(walk.folders.at(0).files.at(0).virtualName, QByteArray("a.mp3"));
    ASSERT_EQ(walk.unreadable.size(), 1);
    EXPECT_TRUE(walk.unreadable.at(0).first.endsWith(u"/b.mp3"_s));
    EXPECT_TRUE(walk.unreadable.at(0).second.contains(u"iCloud"_s));
#endif
}

TEST(ShareBuild, ACancelledBuildSharesNothing)
{
    Tree tree;
    tree.file(u"a.mp3"_s);
    selecta::audio::CancellationFlag cancel;
    cancel.cancel();
    const BuildResult result = build({{tree.root(), u"Music"_s}}, {}, {}, &cancel);
    EXPECT_TRUE(result.cancelled);
    ASSERT_NE(result.index, nullptr);
    EXPECT_EQ(result.index->counts(), (ShareIndex::Counts{0, 0}));
    EXPECT_TRUE(decodedList(*result.index).directories.isEmpty());
}

TEST(ShareBuild, CancellingDuringTheWalkStopsIt)
{
    Tree tree;
    for (int i = 0; i < 10; ++i) {
        tree.file(u"%1.mp3"_s.arg(i));
    }
    selecta::audio::CancellationFlag cancel;
    detail::WalkOptions options;
    options.afterStat = [&cancel](const QByteArray &path, struct stat & /*status*/) {
        if (path.endsWith("/3.mp3")) {
            cancel.cancel();
        }
    };
    const detail::Walk walk = walkTree(tree, options, &cancel);
    EXPECT_TRUE(walk.cancelled);
    EXPECT_LT(walk.files, 10);
}

TEST(ShareBuild, TheBuilderHonoursCancellation)
{
    Tree tree;
    tree.file(u"a.mp3"_s);
    const detail::Walk walk = walkTree(tree, {});
    selecta::audio::CancellationFlag cancel;
    cancel.cancel();
    EXPECT_EQ(detail::IndexBuilder::build(walk, {{tree.root(), u"Music"_s}}, &cancel), nullptr);
    EXPECT_NE(detail::IndexBuilder::build(walk, {{tree.root(), u"Music"_s}}, nullptr), nullptr);
}

// --- Attributes ------------------------------------------------------------------------------------------------------

TEST(ShareBuild, AudioAttributesComeFromTheCollection)
{
    Tree tree;
    const QString lossy = tree.file(u"lossy.mp3"_s, test::mp3Cbr320());
    const QString lossless = tree.file(u"lossless.flac"_s, test::flac(16));
    const QString unknownVbr = tree.file(u"unknown.ogg"_s);
    QHash<QString, collection::ScannedFile> known;
    known.insert(lossy, scanned(lossy, {selecta::match::Codec::Mp3, 123, true, 44100, 0}, 61'999));
    known.insert(lossless, scanned(lossless, {selecta::match::Codec::Flac, 2300, std::nullopt, 96000, 24}, 300'000));
    known.insert(unknownVbr, scanned(unknownVbr, {selecta::match::Codec::Vorbis, 160, std::nullopt, 48000, 0}, 1'000));
    const BuildResult result = buildTree(tree, known, {}, Layout::Tracks);

    const std::optional<slsk::FileEntry> mp3 = listedEntry(*result.index, u"Music\\lossy.mp3"_s);
    ASSERT_TRUE(mp3.has_value());
    EXPECT_EQ(mp3->attributes.bitrate, 123U);
    EXPECT_EQ(mp3->attributes.duration, 61U); // whole seconds, truncated
    EXPECT_EQ(mp3->attributes.vbr, true);
    EXPECT_FALSE(mp3->attributes.sampleRate.has_value());
    EXPECT_FALSE(mp3->attributes.bitDepth.has_value());

    const std::optional<slsk::FileEntry> flac = listedEntry(*result.index, u"Music\\lossless.flac"_s);
    ASSERT_TRUE(flac.has_value());
    EXPECT_EQ(flac->attributes.duration, 300U);
    EXPECT_EQ(flac->attributes.sampleRate, 96000U);
    EXPECT_EQ(flac->attributes.bitDepth, 24U);
    EXPECT_FALSE(flac->attributes.bitrate.has_value());
    EXPECT_FALSE(flac->attributes.vbr.has_value());

    const std::optional<slsk::FileEntry> ogg = listedEntry(*result.index, u"Music\\unknown.ogg"_s);
    ASSERT_TRUE(ogg.has_value());
    EXPECT_EQ(ogg->attributes.bitrate, 160U);
    EXPECT_FALSE(ogg->attributes.vbr.has_value()); // unknown, never "constant"
}

TEST(ShareBuild, ACollectionEntryUnderAnotherSpellingIsFound)
{
    Tree tree;
    const QString path = tree.file(u"Caf\u00e9/a.mp3"_s, test::mp3Cbr320());
    QHash<QString, collection::ScannedFile> known;
    known.insert(path.normalized(QString::NormalizationForm_D),
                 scanned(path, {selecta::match::Codec::Mp3, 111, false, 44100, 0}, 5'000));
    const BuildResult result = buildTree(tree, known);
    const std::optional<slsk::FileEntry> entry = listedEntry(*result.index, u"Music\\Caf\u00e9\\a.mp3"_s);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->attributes.bitrate, 111U);
}

TEST(ShareBuild, AStaleCollectionEntryIsProbedInstead)
{
    Tree tree;
    const QString path = tree.file(u"a.mp3"_s, test::mp3Cbr320());
    collection::ScannedFile stale = scanned(path, {selecta::match::Codec::Mp3, 64, false, 22050, 0}, 9'000);
    stale.file.size += 1;
    collection::ScannedFile older = scanned(path, {selecta::match::Codec::Mp3, 64, false, 22050, 0}, 9'000);
    older.file.modifiedMs -= 5'000;
    for (const collection::ScannedFile &entry : {stale, older}) {
        const BuildResult result = buildTree(tree, {{path, entry}}, {}, Layout::Tracks);
        const std::optional<slsk::FileEntry> listed = listedEntry(*result.index, u"Music\\a.mp3"_s);
        ASSERT_TRUE(listed.has_value());
        EXPECT_EQ(listed->attributes.bitrate, 320U);
        EXPECT_EQ(listed->attributes.duration, 2U);
    }
}

TEST(ShareBuild, AudioOutsideTheCollectionIsProbed)
{
    Tree tree;
    tree.file(u"cbr.mp3"_s, test::mp3Cbr320());
    tree.file(u"vbr.mp3"_s, test::mp3Vbr());
    tree.file(u"16.flac"_s, test::flac(16));
    tree.file(u"24.flac"_s, test::flac(24));
    tree.file(u"wave.wav"_s, selecta::testing::makeWav(selecta::testing::sine(440.0, 3.0, 48000)));
    const BuildResult result = buildTree(tree, {}, {}, Layout::Tracks);

    const std::optional<slsk::FileEntry> cbr = listedEntry(*result.index, u"Music\\cbr.mp3"_s);
    ASSERT_TRUE(cbr.has_value());
    EXPECT_EQ(cbr->attributes.bitrate, 320U);
    EXPECT_EQ(cbr->attributes.duration, 2U);
    EXPECT_EQ(cbr->attributes.vbr, false);

    const std::optional<slsk::FileEntry> vbr = listedEntry(*result.index, u"Music\\vbr.mp3"_s);
    ASSERT_TRUE(vbr.has_value());
    EXPECT_EQ(vbr->attributes.vbr, true);
    EXPECT_GT(vbr->attributes.bitrate.value_or(0), 0U);

    const std::optional<slsk::FileEntry> flac16 = listedEntry(*result.index, u"Music\\16.flac"_s);
    ASSERT_TRUE(flac16.has_value());
    EXPECT_EQ(flac16->attributes.bitDepth, 16U);
    EXPECT_EQ(flac16->attributes.sampleRate, 44100U);
    EXPECT_EQ(flac16->attributes.duration, 2U);
    EXPECT_FALSE(flac16->attributes.bitrate.has_value());

    const std::optional<slsk::FileEntry> flac24 = listedEntry(*result.index, u"Music\\24.flac"_s);
    ASSERT_TRUE(flac24.has_value());
    EXPECT_EQ(flac24->attributes.bitDepth, 24U);
    EXPECT_EQ(flac24->attributes.sampleRate, 48000U);

    const std::optional<slsk::FileEntry> wav = listedEntry(*result.index, u"Music\\wave.wav"_s);
    ASSERT_TRUE(wav.has_value());
    EXPECT_EQ(wav->attributes.bitDepth, 16U);
    EXPECT_EQ(wav->attributes.duration, 3U);
}

TEST(ShareBuild, UnreadableAudioIsSharedWithoutAttributes)
{
    Tree tree;
    tree.file(u"broken.mp3"_s, QByteArray(4096, 'x'));
    tree.file(u"cover.jpg"_s, test::mp3Cbr320()); // not audio by name: neither probed nor shared
    const BuildResult result = buildTree(tree, {}, {}, Layout::Tracks);
    const std::optional<slsk::FileEntry> broken = listedEntry(*result.index, u"Music\\broken.mp3"_s);
    ASSERT_TRUE(broken.has_value());
    EXPECT_TRUE(broken->attributes.isEmpty());
    EXPECT_EQ(listedFiles(*result.index), QStringList({u"Music\\broken.mp3"_s}));
}

TEST(ShareBuild, AttributesOutsideTheWireRangeAreLeftOut)
{
    EXPECT_EQ(detail::attributesOf({selecta::match::Codec::Mp3, -5, std::nullopt, 44100, 0}, -1), detail::Attributes{});
    const detail::Attributes huge = detail::attributesOf({selecta::match::Codec::Flac, 0, {}, 96000, 300}, 1'000);
    EXPECT_EQ(huge.bitDepth, 0); // more than a byte can say: dropped
    EXPECT_EQ(huge.sampleRate, 96000U);
    EXPECT_EQ(detail::attributesOf({selecta::match::Codec::Mp3, 320, {}, 44100, 0}, 999).duration, 0U);
}

// --- The share list --------------------------------------------------------------------------------------------------

TEST(ShareBuild, TheShareListDecodesToExactlyWhatWasIndexed)
{
    Tree tree;
    const QString track = tree.file(u"Album/01 Track.MP3"_s, test::mp3Cbr320());
    tree.file(u"Album/02 Track.flac"_s, QByteArray(1234, 'f'));
    tree.file(u"Album/cover.jpg"_s, QByteArray(1234, 'j'));
    tree.file(u"notes.txt"_s, QByteArray(10, 't'));
    QHash<QString, collection::ScannedFile> known;
    known.insert(track, scanned(track, {selecta::match::Codec::Mp3, 320, false, 44100, 0}, 125'400));
    const BuildResult result = buildTree(tree, known);

    slsk::SharedFileList expected;
    const auto entry = [](const char *name, quint64 size, const QString &extension, slsk::FileAttributes attributes) {
        return slsk::FileEntry{slsk::RemotePath{QByteArray(name)}, size, extension, std::move(attributes)};
    };
    slsk::FileAttributes trackAttributes;
    trackAttributes.bitrate = 320;
    trackAttributes.duration = 125;
    trackAttributes.vbr = false;
    expected.directories = {
        slsk::SharedDirectory{slsk::RemotePath::fromText(u"Music"_s), {}},
        slsk::SharedDirectory{
            slsk::RemotePath::fromText(u"Music\\Album"_s),
            {entry("01 Track.MP3", static_cast<quint64>(test::mp3Cbr320().size()), u"mp3"_s, trackAttributes),
             entry("02 Track.flac", 1234, u"flac"_s, {})}},
    };
    EXPECT_EQ(decodedList(*result.index), expected);
}

TEST(ShareBuild, TheShareListIsWhatSlsksOwnEncoderWrites)
{
    // build() writes the list a folder at a time; the bytes must be SharedFileListResponse::encode()'s, to the byte.
    Tree tree;
    tree.file(u"Loose.mp3"_s, test::mp3Cbr320());
    tree.file(u"A/01.mp3"_s, test::mp3Cbr320());
    tree.file(u"C/notes.txt"_s);
    tree.file(u"C/Ünïcödé.flac"_s, test::flac(24));
    const BuildResult result = buildTree(tree, {}, {}, Layout::TracksAndAlbums);
    slsk::peer::SharedFileListResponse response;
    response.list = decodedList(*result.index);
    ASSERT_EQ(response.list.directories.size(), 3);
    EXPECT_EQ(response.encode(), result.index->sharedFileList());
    EXPECT_EQ(ShareIndex().sharedFileList(), slsk::peer::SharedFileListResponse{}.encode());
}

TEST(ShareBuild, AnEmptyIndexSharesNothing)
{
    const ShareIndex empty;
    EXPECT_EQ(empty.counts(), (ShareIndex::Counts{0, 0}));
    EXPECT_EQ(decodedList(empty), slsk::SharedFileList{});
    EXPECT_TRUE(empty.roots().isEmpty());
    EXPECT_GT(empty.memoryBytes(), 0);
    EXPECT_TRUE(empty.search(u"anything at all", {}).isEmpty());
    EXPECT_FALSE(empty.lookup(slsk::RemotePath::fromText(u"Music\\a.mp3"_s)).has_value());
    EXPECT_TRUE(empty.folderContents(slsk::RemotePath::fromText(u"Music"_s)).isEmpty());

    Tree tree;
    const BuildResult nothing = buildTree(tree);
    EXPECT_FALSE(nothing.cancelled);
    EXPECT_EQ(nothing.index->counts(), (ShareIndex::Counts{0, 0}));
    EXPECT_EQ(nothing.index->roots().size(), 1);
}

TEST(ShareBuild, MemoryIsAccountedFor)
{
    Tree tree;
    for (int i = 0; i < 50; ++i) {
        tree.file(u"Folder %1/Track %2.mp3"_s.arg(i % 5).arg(i));
    }
    const BuildResult result = buildTree(tree);
    EXPECT_EQ(result.index->counts().files, 50);
    EXPECT_GT(result.index->memoryBytes(), result.index->sharedFileList().size());
}

} // namespace
