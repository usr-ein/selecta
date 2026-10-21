// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/FileOps.h"

#include "library/Quarantine.h"
#include "support/QuarantineSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <filesystem>
#include <gtest/gtest.h>
#include <system_error>

using namespace Qt::StringLiterals;
using selecta::library::FileOps;
namespace quarantine = selecta::library::quarantine;

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Whether a folder without write permission refuses this user's writes: not on Windows, where the read-only attribute
// doesn't apply to folders.
#ifdef Q_OS_WIN
constexpr bool kReadOnlyFoldersStopWrites = false;
#else
constexpr bool kReadOnlyFoldersStopWrites = true;
#endif

// Makes `link` a symbolic link to the folder `target`. On Windows QFile::link makes a shortcut (.lnk), which is no link
// at all, and only an administrator or Developer Mode may make a real one: there a test without one skips.
#ifdef Q_OS_WIN
constexpr bool kLinksNeedPrivilege = true;
bool linkFolder(const QString &target, const QString &link)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(QDir::toNativeSeparators(target).toStdWString(),
                                              QDir::toNativeSeparators(link).toStdWString(), error);
    return !error;
}
#else
constexpr bool kLinksNeedPrivilege = false;
bool linkFolder(const QString &target, const QString &link)
{
    return QFile::link(target, link);
}
#endif

class FileOpsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        inbox = dir.filePath(u"Inbox"_s);
        collection = dir.filePath(u"Music"_s);
        incoming = dir.filePath(u"incoming"_s);
        ASSERT_TRUE(QDir().mkpath(inbox) && QDir().mkpath(collection) && QDir().mkpath(incoming));
    }

    FileOps ops()
    {
        return FileOps(inbox, {collection}, [this](const QString &path, QString *trashed) {
            trashedPaths.append(path);
            const QString target = dir.filePath(u"fake-trash/"_s) + QFileInfo(path).fileName();
            QDir().mkpath(QFileInfo(target).absolutePath());
            *trashed = target;
            return QFile::rename(path, target);
        });
    }

    QTemporaryDir dir;
    QString inbox;
    QString collection;
    QString incoming;
    QStringList trashedPaths;
};

} // namespace

TEST_F(FileOpsTest, PublishesIntoTheInbox)
{
    const QString temp = incoming + u"/a.mp3"_s;
    ASSERT_TRUE(writeFile(temp, "audio"));
    const auto result = ops().publishIntoInbox(temp, u"Singles/Artist - Title.mp3"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(inbox + u"/Singles/Artist - Title.mp3"_s), "audio");
    EXPECT_FALSE(QFileInfo::exists(temp));
}

TEST_F(FileOpsTest, PublishingNeverOverwritesItAddsANumber)
{
    ASSERT_TRUE(writeFile(inbox + u"/Singles/Song.mp3"_s, "old"));
    ASSERT_TRUE(writeFile(inbox + u"/Singles/Song (2).mp3"_s, "older"));
    ASSERT_TRUE(writeFile(incoming + u"/new.mp3"_s, "new"));
    const auto result = ops().publishIntoInbox(incoming + u"/new.mp3"_s, u"Singles/Song.mp3"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.path.endsWith(u"Singles/Song (3).mp3"_s)) << result.path.toStdString();
    EXPECT_EQ(readFile(inbox + u"/Singles/Song.mp3"_s), "old");
}

TEST_F(FileOpsTest, PublishingRefusesPathsOutsideTheInbox)
{
    ASSERT_TRUE(writeFile(incoming + u"/a.mp3"_s, "x"));
    EXPECT_FALSE(ops().publishIntoInbox(incoming + u"/a.mp3"_s, u"../Music/escape.mp3"_s).ok);
    EXPECT_FALSE(ops().publishIntoInbox(incoming + u"/a.mp3"_s, dir.filePath(u"abs.mp3"_s)).ok);
    EXPECT_FALSE(ops().publishIntoInbox(incoming + u"/a.mp3"_s, QString()).ok);
    EXPECT_FALSE(ops().publishIntoInbox(incoming + u"/missing.mp3"_s, u"x.mp3"_s).ok);
    EXPECT_TRUE(QFileInfo::exists(incoming + u"/a.mp3"_s)); // nothing moved
}

TEST_F(FileOpsTest, SymlinkCantEscapeTheInbox)
{
    const bool linked = linkFolder(collection, inbox + u"/sneaky"_s);
    if (!linked && kLinksNeedPrivilege) {
        GTEST_SKIP() << "this user can't make symbolic links";
    }
    ASSERT_TRUE(linked);
    ASSERT_TRUE(writeFile(incoming + u"/a.mp3"_s, "x"));
    EXPECT_FALSE(ops().publishIntoInbox(incoming + u"/a.mp3"_s, u"sneaky/a.mp3"_s).ok);
    EXPECT_FALSE(QFileInfo::exists(collection + u"/a.mp3"_s));
}

TEST_F(FileOpsTest, FilesIntoTheCollectionAndLeavesTheInbox)
{
    ASSERT_TRUE(writeFile(inbox + u"/Singles/Song.mp3"_s, "audio"));
    const auto result = ops().fileIntoCollection(inbox + u"/Singles/Song.mp3"_s, collection, u"Singles/Song.mp3"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(collection + u"/Singles/Song.mp3"_s), "audio");
    EXPECT_FALSE(QFileInfo::exists(inbox + u"/Singles/Song.mp3"_s));
}

TEST_F(FileOpsTest, NeverOverwritesACollectionFile)
{
    ASSERT_TRUE(writeFile(collection + u"/Singles/Song.mp3"_s, "precious"));
    ASSERT_TRUE(writeFile(inbox + u"/Song.mp3"_s, "new"));
    const auto result = ops().fileIntoCollection(inbox + u"/Song.mp3"_s, collection, u"Singles/Song.mp3"_s);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(readFile(collection + u"/Singles/Song.mp3"_s), "precious");
    EXPECT_TRUE(QFileInfo::exists(inbox + u"/Song.mp3"_s));
}

TEST_F(FileOpsTest, AFailedMoveSaysWhyInWords)
{
    if (!kReadOnlyFoldersStopWrites || selecta::testing::runningAsRoot()) {
        GTEST_SKIP() << "needs a folder that refuses this user's writes";
    }
    ASSERT_TRUE(writeFile(inbox + u"/Song.mp3"_s, "new"));
    const QString singles = collection + u"/Singles"_s;
    ASSERT_TRUE(QDir().mkpath(singles));
    ASSERT_TRUE(QFile::setPermissions(singles, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto result = ops().fileIntoCollection(inbox + u"/Song.mp3"_s, collection, u"Singles/Song.mp3"_s);
    QFile::setPermissions(singles, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    EXPECT_FALSE(result.ok);
    // The system's words for EACCES, on Linux too (glibc's strerror_r returns its message rather than filling ours).
    EXPECT_TRUE(result.error.contains(u"Permission denied"_s)) << result.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(inbox + u"/Song.mp3"_s));
}

TEST_F(FileOpsTest, CrossVolumeCopyIsVerifiedAndAtomic)
{
    FileOps copying = ops();
    copying.setForceCopy(true);
    const QByteArray content((3 * 1024 * 1024) + 17, 'z');
    ASSERT_TRUE(writeFile(inbox + u"/big.flac"_s, content));
    const auto result = copying.fileIntoCollection(inbox + u"/big.flac"_s, collection, u"Albums/A/big.flac"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(collection + u"/Albums/A/big.flac"_s), content);
    EXPECT_FALSE(QFileInfo::exists(collection + u"/Albums/A/big.flac.selecta-part"_s));
    EXPECT_FALSE(QFileInfo::exists(inbox + u"/big.flac"_s));
}

TEST_F(FileOpsTest, OnlyInboxFilesCanBeFiledAndOnlyIntoKnownRoots)
{
    ASSERT_TRUE(writeFile(collection + u"/already.mp3"_s, "x"));
    EXPECT_FALSE(ops().fileIntoCollection(collection + u"/already.mp3"_s, collection, u"moved.mp3"_s).ok);
    ASSERT_TRUE(writeFile(inbox + u"/a.mp3"_s, "x"));
    EXPECT_FALSE(ops().fileIntoCollection(inbox + u"/a.mp3"_s, dir.filePath(u"elsewhere"_s), u"a.mp3"_s).ok);
    EXPECT_FALSE(ops().fileIntoCollection(inbox + u"/a.mp3"_s, collection, u"../escape.mp3"_s).ok);
    EXPECT_TRUE(QFileInfo::exists(inbox + u"/a.mp3"_s));
}

TEST_F(FileOpsTest, TrashOnlyFromTheInbox)
{
    ASSERT_TRUE(writeFile(inbox + u"/Singles/old.mp3"_s, "x"));
    ASSERT_TRUE(writeFile(collection + u"/keep.mp3"_s, "x"));
    const auto trashed = ops().trashFromInbox(inbox + u"/Singles/old.mp3"_s);
    ASSERT_TRUE(trashed.ok) << trashed.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(inbox + u"/Singles/old.mp3"_s));

    EXPECT_FALSE(ops().trashFromInbox(collection + u"/keep.mp3"_s).ok);
    EXPECT_FALSE(ops().trashFromInbox(inbox).ok); // the Inbox root itself
    EXPECT_FALSE(ops().trashFromInbox(inbox + u"/missing.mp3"_s).ok);
    EXPECT_TRUE(QFileInfo::exists(collection + u"/keep.mp3"_s));
    EXPECT_EQ(trashedPaths.size(), 1);
}

TEST(FileOps, IsInsideHandlesPrefixesAndDots)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(u"Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(root));
    EXPECT_TRUE(FileOps::isInside(root + u"/a/b.mp3"_s, root));
    EXPECT_FALSE(FileOps::isInside(root, root));
    EXPECT_FALSE(FileOps::isInside(root + u"Other/x.mp3"_s, root)); // "Inbox" vs "InboxOther"
    EXPECT_FALSE(FileOps::isInside(root + u"/../x.mp3"_s, root));
}

TEST_F(FileOpsTest, FilesAnAlbumFolderWhole)
{
    const QString album = inbox + u"/Albums/Okuma Drift - Nightfall (2024)"_s;
    ASSERT_TRUE(writeFile(album + u"/01 - Neon Rain.mp3"_s, "one"));
    ASSERT_TRUE(writeFile(album + u"/02 - Sodium.mp3"_s, "two"));
    ASSERT_TRUE(writeFile(album + u"/cover.jpg"_s, "jpeg"));
    const auto result = ops().fileFolderIntoCollection(album, collection, u"Albums/Okuma Drift - Nightfall (2024)"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    const QString filed = collection + u"/Albums/Okuma Drift - Nightfall (2024)"_s;
    EXPECT_EQ(readFile(filed + u"/01 - Neon Rain.mp3"_s), "one");
    EXPECT_EQ(readFile(filed + u"/cover.jpg"_s), "jpeg");
    EXPECT_FALSE(QFileInfo::exists(album));
}

TEST_F(FileOpsTest, AlbumCopyAcrossVolumesIsStagedAndVerified)
{
    FileOps copying = ops();
    copying.setForceCopy(true);
    const QString album = inbox + u"/Albums/A"_s;
    ASSERT_TRUE(writeFile(album + u"/01.flac"_s, QByteArray(qsizetype{2} * 1024 * 1024, 'a')));
    ASSERT_TRUE(writeFile(album + u"/CD2/01.flac"_s, "disc two"));
    // A staging folder left by a crashed attempt is cleared, not merged.
    ASSERT_TRUE(writeFile(collection + u"/Albums/.A.selecta-part/stale.flac"_s, "stale"));

    const auto result = copying.fileFolderIntoCollection(album, collection, u"Albums/A"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(collection + u"/Albums/A/CD2/01.flac"_s), "disc two");
    EXPECT_EQ(readFile(collection + u"/Albums/A/01.flac"_s).size(), qsizetype{2} * 1024 * 1024);
    EXPECT_FALSE(QFileInfo::exists(collection + u"/Albums/A/stale.flac"_s));
    EXPECT_FALSE(QFileInfo::exists(collection + u"/Albums/.A.selecta-part"_s));
    EXPECT_FALSE(QFileInfo::exists(album));
}

TEST_F(FileOpsTest, AlbumFilingNeverMergesIntoAnExistingFolder)
{
    ASSERT_TRUE(writeFile(collection + u"/Albums/A/01.mp3"_s, "precious"));
    ASSERT_TRUE(writeFile(inbox + u"/Albums/A/01.mp3"_s, "new"));
    const auto result = ops().fileFolderIntoCollection(inbox + u"/Albums/A"_s, collection, u"Albums/A"_s);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(readFile(collection + u"/Albums/A/01.mp3"_s), "precious");
    EXPECT_TRUE(QFileInfo::exists(inbox + u"/Albums/A/01.mp3"_s));
}

TEST_F(FileOpsTest, AlbumWithASymlinkIsRefused)
{
    ASSERT_TRUE(writeFile(inbox + u"/Albums/A/01.mp3"_s, "x"));
    const bool linked = linkFolder(collection, inbox + u"/Albums/A/escape"_s);
    if (!linked && kLinksNeedPrivilege) {
        GTEST_SKIP() << "this user can't make symbolic links";
    }
    ASSERT_TRUE(linked);
    EXPECT_FALSE(ops().fileFolderIntoCollection(inbox + u"/Albums/A"_s, collection, u"Albums/A"_s).ok);
    EXPECT_FALSE(QFileInfo::exists(collection + u"/Albums/A"_s));
}

TEST_F(FileOpsTest, OnlyInboxFoldersAreFiled)
{
    ASSERT_TRUE(writeFile(incoming + u"/A/01.mp3"_s, "x"));
    EXPECT_FALSE(ops().fileFolderIntoCollection(incoming + u"/A"_s, collection, u"Albums/A"_s).ok);
    EXPECT_FALSE(ops().fileFolderIntoCollection(inbox, collection, u"Albums/Everything"_s).ok); // the Inbox itself
    ASSERT_TRUE(writeFile(inbox + u"/Albums/A/01.mp3"_s, "x"));
    EXPECT_FALSE(ops().fileFolderIntoCollection(inbox + u"/Albums/A"_s, collection, u"../A"_s).ok);
    EXPECT_FALSE(ops().fileFolderIntoCollection(inbox + u"/Albums/A"_s, dir.filePath(u"other"_s), u"A"_s).ok);
}

TEST_F(FileOpsTest, PublishesAStagedAlbumIntoTheInbox)
{
    const QString staged = incoming + u"/album-7"_s;
    ASSERT_TRUE(writeFile(staged + u"/01 - One.mp3"_s, "one"));
    ASSERT_TRUE(writeFile(staged + u"/cover.jpg"_s, "cover"));
    const auto result = ops().publishFolderIntoInbox(staged, u"Albums/Mr. Oizo - Analog Worms Attack (1999)"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(inbox + u"/Albums/Mr. Oizo - Analog Worms Attack (1999)/01 - One.mp3"_s), "one");
    EXPECT_FALSE(QFileInfo::exists(staged));

    // The same name again: a number at the end, never inside the name's dots.
    ASSERT_TRUE(writeFile(staged + u"/01 - One.mp3"_s, "again"));
    const auto second = ops().publishFolderIntoInbox(staged, u"Albums/Mr. Oizo - Analog Worms Attack (1999)"_s);
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_TRUE(second.path.endsWith(u"Albums/Mr. Oizo - Analog Worms Attack (1999) (2)"_s))
        << second.path.toStdString();
}

TEST_F(FileOpsTest, PublishingAnAlbumAcrossVolumesCopiesAndVerifies)
{
    FileOps fileOps = ops();
    fileOps.setForceCopy(true);
    const QString staged = incoming + u"/album-8"_s;
    ASSERT_TRUE(writeFile(staged + u"/01 - One.mp3"_s, "one"));
    const auto result = fileOps.publishFolderIntoInbox(staged, u"Albums/Artist - Album"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(inbox + u"/Albums/Artist - Album/01 - One.mp3"_s), "one");
    EXPECT_FALSE(QFileInfo::exists(staged));
    EXPECT_FALSE(fileOps.publishFolderIntoInbox(staged, u"../outside"_s).ok);
}

TEST_F(FileOpsTest, WhatIsPublishedIntoTheInboxIsQuarantined)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    for (const bool copying : {false, true}) { // renamed into place, or copied across volumes
        FileOps fileOps = ops();
        fileOps.setForceCopy(copying);
        const QString suffix = copying ? u"copied"_s : u"renamed"_s;

        // A single.
        const QString temp = incoming + u"/a.mp3"_s;
        ASSERT_TRUE(writeFile(temp, "audio"));
        const auto single = fileOps.publishIntoInbox(temp, u"Singles/%1.mp3"_s.arg(suffix));
        ASSERT_TRUE(single.ok) << single.error.toStdString();
        EXPECT_EQ(readFile(single.path), "audio");
        EXPECT_EQ(quarantine::agentOf(single.path), u"Selecta"_s) << suffix.toStdString();

        // An album: the folder and everything in it.
        const QString staged = incoming + u"/album-"_s + suffix;
        ASSERT_TRUE(writeFile(staged + u"/01 - One.mp3"_s, "one"));
        ASSERT_TRUE(writeFile(staged + u"/cover.jpg"_s, "cover"));
        ASSERT_TRUE(writeFile(staged + u"/CD2/01 - Two.mp3"_s, "two"));
        const auto album = fileOps.publishFolderIntoInbox(staged, u"Albums/"_s + suffix);
        ASSERT_TRUE(album.ok) << album.error.toStdString();
        const QStringList paths{album.path, album.path + u"/01 - One.mp3"_s, album.path + u"/cover.jpg"_s,
                                album.path + u"/CD2"_s, album.path + u"/CD2/01 - Two.mp3"_s};
        for (const QString &path : paths) {
            EXPECT_EQ(quarantine::agentOf(path), u"Selecta"_s) << path.toStdString();
        }
    }
}

TEST_F(FileOpsTest, AQuarantineAttributeAlreadyThereIsKeptOnTheWayIn)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    for (const bool copying : {false, true}) {
        FileOps fileOps = ops();
        fileOps.setForceCopy(copying);
        const QString temp = incoming + u"/a.mp3"_s;
        ASSERT_TRUE(writeFile(temp, "audio"));
        ASSERT_TRUE(selecta::testing::setQuarantine(temp, selecta::testing::safariApproved()));
        const auto result = fileOps.publishIntoInbox(temp, u"Singles/A.mp3"_s);
        ASSERT_TRUE(result.ok) << result.error.toStdString();
        EXPECT_EQ(quarantine::value(result.path), selecta::testing::safariApproved());
    }
}

TEST_F(FileOpsTest, AFileThatCantBeQuarantinedStillArrives)
{
    if (!quarantine::isSupported() || selecta::testing::runningAsRoot()) {
        GTEST_SKIP() << "needs the attribute, and a user that read-only files stop";
    }
    const QString temp = incoming + u"/locked.mp3"_s;
    ASSERT_TRUE(writeFile(temp, "audio"));
    ASSERT_TRUE(QFile::setPermissions(temp, QFileDevice::ReadOwner | QFileDevice::ReadGroup));
    const auto result = ops().publishIntoInbox(temp, u"Singles/Locked.mp3"_s);
    ASSERT_TRUE(result.ok) << result.error.toStdString(); // the log says it has no attribute
    EXPECT_EQ(readFile(result.path), "audio");
    EXPECT_TRUE(quarantine::value(result.path).isEmpty());
}

TEST_F(FileOpsTest, FilingKeepsTheQuarantineAttributeAndNeverAddsOne)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    for (const bool copying : {false, true}) {
        FileOps fileOps = ops();
        fileOps.setForceCopy(copying);
        const QString name = copying ? u"copied"_s : u"renamed"_s;

        // A quarantined single keeps exactly its attribute in the collection, and stays byte for byte the same.
        const QString single = inbox + u"/Singles/"_s + name + u".mp3"_s;
        ASSERT_TRUE(writeFile(single, "audio"));
        ASSERT_TRUE(quarantine::mark(single));
        const QByteArray attribute = quarantine::value(single);
        const auto filed = fileOps.fileIntoCollection(single, collection, u"Singles/"_s + name + u".mp3"_s);
        ASSERT_TRUE(filed.ok) << filed.error.toStdString();
        EXPECT_EQ(readFile(filed.path), "audio");
        EXPECT_EQ(quarantine::value(filed.path), attribute) << name.toStdString();
        EXPECT_FALSE(QFileInfo::exists(single));

        // A file that came without one (from before Selecta set it) is filed as it is: nothing is added.
        const QString plain = inbox + u"/Singles/plain-"_s + name + u".mp3"_s;
        ASSERT_TRUE(writeFile(plain, "plain"));
        const auto filedPlain = fileOps.fileIntoCollection(plain, collection, u"Singles/plain-"_s + name + u".mp3"_s);
        ASSERT_TRUE(filedPlain.ok) << filedPlain.error.toStdString();
        EXPECT_TRUE(quarantine::value(filedPlain.path).isEmpty());

        // An album, with every file and folder in it.
        const QString album = inbox + u"/Albums/"_s + name;
        ASSERT_TRUE(writeFile(album + u"/01.mp3"_s, "one"));
        ASSERT_TRUE(writeFile(album + u"/CD2/01.mp3"_s, "two"));
        ASSERT_TRUE(quarantine::markTree(album));
        const auto filedAlbum = fileOps.fileFolderIntoCollection(album, collection, u"Albums/"_s + name);
        ASSERT_TRUE(filedAlbum.ok) << filedAlbum.error.toStdString();
        const QStringList paths{filedAlbum.path, filedAlbum.path + u"/01.mp3"_s, filedAlbum.path + u"/CD2"_s,
                                filedAlbum.path + u"/CD2/01.mp3"_s};
        for (const QString &path : paths) {
            EXPECT_EQ(quarantine::agentOf(path), u"Selecta"_s) << path.toStdString();
        }
        EXPECT_EQ(readFile(filedAlbum.path + u"/CD2/01.mp3"_s), "two");
        EXPECT_FALSE(QFileInfo::exists(album));
    }
}

TEST_F(FileOpsTest, AReplacedInboxFileKeepsItsQuarantineAttribute)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    // Re-tagging: a copy made by reading and writing (no attribute) takes the place of the quarantined file.
    const QString file = inbox + u"/Singles/Song.mp3"_s;
    const QString copy = inbox + u"/Singles/.Song.mp3.selecta-part"_s;
    ASSERT_TRUE(writeFile(file, "old tags") && writeFile(copy, "new tags"));
    ASSERT_TRUE(selecta::testing::setQuarantine(file, selecta::testing::safariApproved()));
    const auto result = ops().replaceInInbox(copy, file);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(file), "new tags");
    EXPECT_EQ(quarantine::value(file), selecta::testing::safariApproved());

    // One that had none (from before Selecta set it) gets Selecta's: it is a download all the same.
    const QString older = inbox + u"/Singles/Older.mp3"_s;
    const QString olderCopy = inbox + u"/Singles/.Older.mp3.selecta-part"_s;
    ASSERT_TRUE(writeFile(older, "old") && writeFile(olderCopy, "new"));
    ASSERT_TRUE(ops().replaceInInbox(olderCopy, older).ok);
    EXPECT_EQ(quarantine::agentOf(older), u"Selecta"_s);
}

TEST_F(FileOpsTest, ANewFilePlacedAcrossVolumesKeepsItsQuarantineAttribute)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    FileOps fileOps = ops();
    fileOps.setForceCopy(true);
    const QString staged = incoming + u"/converted.mp3"_s;
    ASSERT_TRUE(writeFile(staged, "mp3"));
    ASSERT_TRUE(selecta::testing::setQuarantine(staged, selecta::testing::safariApproved()));
    const auto placed = fileOps.placeNewFile(staged, dir.filePath(u"converted.mp3"_s));
    ASSERT_TRUE(placed.ok) << placed.error.toStdString();
    EXPECT_EQ(quarantine::value(placed.path), selecta::testing::safariApproved());
}

// --- Upgrades (P-010, PRD D41) ---------------------------------------------------------------------------------------

namespace {

// What a folder holds, hidden files included: no leftovers from staging.
QStringList entriesOf(const QString &folder)
{
    return QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

} // namespace

TEST_F(FileOpsTest, AnUpgradeTakesTheOldFilesExactPlace)
{
    const QString album = collection + u"/Albums/Okuma Drift - Nightfall"_s;
    const QString old = album + u"/01 Dusk.mp3"_s;
    const QString upgrade = inbox + u"/Singles/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old 128") && writeFile(upgrade, "new 320"));
    const auto result = ops().replaceInCollection(upgrade, old);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(result.path, QFileInfo(old).canonicalFilePath()); // reported as filed paths are: real ones
    EXPECT_EQ(readFile(old), "new 320");
    EXPECT_FALSE(QFileInfo::exists(upgrade));
    ASSERT_EQ(trashedPaths.size(), 1);
    EXPECT_EQ(QFileInfo(trashedPaths.constFirst()).fileName(), u"01 Dusk.mp3"_s);
    EXPECT_EQ(readFile(dir.filePath(u"fake-trash/01 Dusk.mp3"_s)), "old 128"); // the old file, under its own name
    EXPECT_EQ(entriesOf(album), QStringList{u"01 Dusk.mp3"_s});
}

TEST_F(FileOpsTest, AnUpgradeOfAFileNoLongerThereTakesItsPlace)
{
    const QString old = collection + u"/Tracks/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(old).absolutePath()));
    const QString upgrade = inbox + u"/Singles/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(upgrade, "new"));
    const auto result = ops().replaceInCollection(upgrade, old);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(readFile(old), "new");
    EXPECT_TRUE(trashedPaths.isEmpty());
}

TEST_F(FileOpsTest, AnUpgradeKeepsTheNameAndExtension)
{
    const QString old = collection + u"/Tracks/Okuma Drift - Dusk.m4a"_s;
    const QString upgrade = inbox + u"/Singles/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "aac") && writeFile(upgrade, "mp3"));
    const auto refused = ops().replaceInCollection(upgrade, old);
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.error.contains(u"same type"_s)) << refused.error.toStdString();
    EXPECT_FALSE(ops().checkReplace(upgrade, old).ok);
    EXPECT_EQ(readFile(old), "aac"); // nothing changed
    EXPECT_EQ(readFile(upgrade), "mp3");
    EXPECT_TRUE(trashedPaths.isEmpty());

    // The extension's case doesn't make another type.
    const QString upper = collection + u"/Tracks/Okuma Drift - Dawn.MP3"_s;
    const QString lower = inbox + u"/Singles/Okuma Drift - Dawn.mp3"_s;
    ASSERT_TRUE(writeFile(upper, "old") && writeFile(lower, "new"));
    ASSERT_TRUE(ops().replaceInCollection(lower, upper).ok);
    EXPECT_EQ(readFile(upper), "new");
}

TEST_F(FileOpsTest, AnUpgradeOnlyReplacesAPlainCollectionFile)
{
    const QString upgrade = inbox + u"/Singles/Song.mp3"_s;
    ASSERT_TRUE(writeFile(upgrade, "new"));
    // Outside the collection folders, relative, in a folder that isn't there, a folder, a symbolic link.
    ASSERT_TRUE(writeFile(incoming + u"/Song.mp3"_s, "elsewhere"));
    EXPECT_FALSE(ops().replaceInCollection(upgrade, incoming + u"/Song.mp3"_s).ok);
    EXPECT_FALSE(ops().replaceInCollection(upgrade, u"Tracks/Song.mp3"_s).ok);
    EXPECT_FALSE(ops().replaceInCollection(upgrade, collection + u"/Gone/Song.mp3"_s).ok);
    ASSERT_TRUE(QDir().mkpath(collection + u"/Tracks/Folder.mp3"_s));
    EXPECT_FALSE(ops().replaceInCollection(upgrade, collection + u"/Tracks/Folder.mp3"_s).ok);
    ASSERT_TRUE(QFile::link(incoming + u"/Song.mp3"_s, collection + u"/Tracks/Link.mp3"_s));
    EXPECT_FALSE(ops().replaceInCollection(upgrade, collection + u"/Tracks/Link.mp3"_s).ok);
    EXPECT_EQ(readFile(incoming + u"/Song.mp3"_s), "elsewhere");
    // Only an Inbox file replaces anything.
    ASSERT_TRUE(writeFile(collection + u"/Tracks/Old.mp3"_s, "old"));
    EXPECT_FALSE(ops().replaceInCollection(incoming + u"/Song.mp3"_s, collection + u"/Tracks/Old.mp3"_s).ok);
    EXPECT_EQ(readFile(collection + u"/Tracks/Old.mp3"_s), "old");
    EXPECT_TRUE(trashedPaths.isEmpty());
    EXPECT_EQ(readFile(upgrade), "new");
}

TEST_F(FileOpsTest, AnUpgradeWhoseOldFileCantGoToTheTrashChangesNothing)
{
    const QString old = collection + u"/Tracks/Song.mp3"_s;
    const QString upgrade = inbox + u"/Singles/Song.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old") && writeFile(upgrade, "new"));
    const FileOps refusing(inbox, {collection}, [](const QString &, QString *) { return false; });
    const auto result = refusing.replaceInCollection(upgrade, old);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(u"nothing changed"_s)) << result.error.toStdString();
    EXPECT_EQ(readFile(old), "old");
    EXPECT_EQ(readFile(upgrade), "new");
    EXPECT_EQ(entriesOf(collection + u"/Tracks"_s), QStringList{u"Song.mp3"_s}); // the staged copy went
}

// Should the copy fail to take the old file's place, the old file comes back from the Trash.
TEST_F(FileOpsTest, AnUpgradeThatCantTakeThePlacePutsTheOldFileBack)
{
    const QString tracks = collection + u"/Tracks"_s;
    const QString old = tracks + u"/Song.mp3"_s;
    const QString upgrade = inbox + u"/Singles/Song.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old") && writeFile(upgrade, "new"));
    const QString trashFolder = dir.filePath(u"fake-trash"_s);
    ASSERT_TRUE(QDir().mkpath(trashFolder));
    // The Trash takes the file, and the staged copy vanishes meanwhile (another app, a full disk…).
    const FileOps vanishing(inbox, {collection}, [&](const QString &path, QString *trashed) {
        *trashed = trashFolder + u"/Song.mp3"_s;
        QFile::remove(QFileInfo(path).dir().filePath(u".Song.mp3.selecta-part"_s));
        return QFile::rename(path, *trashed);
    });
    const auto result = vanishing.replaceInCollection(upgrade, old);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(u"the old file is back"_s)) << result.error.toStdString();
    EXPECT_EQ(readFile(old), "old");
    EXPECT_EQ(readFile(upgrade), "new"); // still in the Inbox, to file again
    EXPECT_EQ(entriesOf(tracks), QStringList{u"Song.mp3"_s});
}

TEST_F(FileOpsTest, AnUpgradeKeepsItsQuarantineAttribute)
{
    if (!quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    const QString old = collection + u"/Tracks/Song.mp3"_s;
    const QString upgrade = inbox + u"/Singles/Song.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old") && writeFile(upgrade, "new"));
    ASSERT_TRUE(selecta::testing::setQuarantine(upgrade, selecta::testing::safariApproved()));
    ASSERT_TRUE(ops().replaceInCollection(upgrade, old).ok);
    EXPECT_EQ(quarantine::value(old), selecta::testing::safariApproved());
}
