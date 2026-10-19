// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/FileOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::library::FileOps;

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
    ASSERT_TRUE(QFile::link(collection, inbox + u"/sneaky"_s));
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
    ASSERT_TRUE(QFile::link(collection, inbox + u"/Albums/A/escape"_s));
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
