// SPDX-License-Identifier: GPL-3.0-or-later
// ShareIndex::lookup() and folderContents(): the only ways from a peer's string to a shared file or folder.
#include "share/ShareIndex.h"

#include "ShareTestSupport.h"
#include "slsk/PeerMessages.h"

#include <QDir>
#include <QFile>

#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace Qt::StringLiterals;
using selecta::share::BuildResult;
using selecta::share::SharedFile;
using selecta::share::ShareIndex;
using selecta::share::test::buildTree;
using selecta::share::test::Tree;
namespace slsk = selecta::slsk;
namespace test = selecta::share::test;

namespace {

slsk::RemotePath remote(const QString &text)
{
    return slsk::RemotePath::fromText(text);
}

quint64 inodeOf(const QString &path)
{
    struct stat status{};
    return ::stat(QFile::encodeName(path).constData(), &status) == 0 ? static_cast<quint64>(status.st_ino) : 0;
}

class ShareLookup : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_track = m_tree.file(u"Artist/Album/01 Track.mp3"_s, QByteArrayLiteral("track bytes"));
        m_tree.file(u"Artist/Album/cover.jpg"_s, QByteArrayLiteral("jpg"));
        m_tree.file(u"Artist/Other/02.flac"_s);
        m_result = buildTree(m_tree);
    }

    const ShareIndex &index() const { return *m_result.index; }

    Tree m_tree;
    QString m_track;
    BuildResult m_result;
};

// --- lookup() --------------------------------------------------------------------------------------------------------

TEST_F(ShareLookup, FindsAFileByItsExactVirtualPath)
{
    const std::optional<SharedFile> file = index().lookup(remote(u"Music\\Artist\\Album\\01 Track.mp3"_s));
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(QDir::cleanPath(file->localPath), QDir::cleanPath(m_track));
    EXPECT_EQ(file->size, 11U);
    EXPECT_EQ(file->inode, inodeOf(m_track));
    EXPECT_NE(file->device, 0U);
    EXPECT_EQ(file->entry.name.display(), u"Music\\Artist\\Album\\01 Track.mp3"_s);
    EXPECT_EQ(file->entry.size, 11U);
    EXPECT_EQ(file->entry.extension, u"mp3"_s);
}

TEST_F(ShareLookup, FallsBackToIgnoringCase)
{
    // Soulseek NS lower-cases whole paths.
    const std::optional<SharedFile> file = index().lookup(remote(u"music\\artist\\album\\01 track.mp3"_s));
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->entry.name.display(), u"Music\\Artist\\Album\\01 Track.mp3"_s);
    EXPECT_TRUE(index().lookup(remote(u"MUSIC\\ARTIST\\OTHER\\02.FLAC"_s)).has_value());
}

TEST_F(ShareLookup, FindsNothingElse)
{
    for (const QString &path :
         {u"Music\\Artist\\Album\\missing.mp3"_s, u"Music\\Artist\\Album"_s, u"Music\\Artist\\Album\\"_s,
          u"Music"_s, u"\\Music\\Artist\\Album\\01 Track.mp3"_s, u"Music/Artist/Album/01 Track.mp3"_s,
          u"Music\\Artist\\..\\Artist\\Album\\01 Track.mp3"_s, u"Other\\Artist\\Album\\01 Track.mp3"_s,
          m_track, u"01 Track.mp3"_s, QString()}) {
        EXPECT_FALSE(index().lookup(remote(path)).has_value()) << path.toStdString();
    }
}

TEST_F(ShareLookup, PathsOverTheLimitFindNothing)
{
    const QString padding(selecta::share::kMaxRequestPathBytes, u'x');
    EXPECT_FALSE(index().lookup(remote(u"Music\\Artist\\Album\\"_s + padding)).has_value());
    EXPECT_TRUE(index().folderContents(remote(u"Music\\"_s + padding)).isEmpty());
}

TEST(ShareLookupNames, NormalisationAndOldClientsAreHandled)
{
    Tree tree;
    ASSERT_TRUE(QDir().mkpath(tree.path(u"Album"_s)));
    const QByteArray nfdPath = tree.nativeFile(u"Album/Cafe\u0301.mp3"_s.toUtf8(), QByteArrayLiteral("nfd"));
    const BuildResult result = buildTree(tree);
    const ShareIndex &index = *result.index;

    // NFC from a peer, NFD from a peer, Latin-1 bytes from an old client: the same file.
    const std::optional<SharedFile> nfc = index.lookup(remote(u"Music\\Album\\Caf\u00e9.mp3"_s));
    ASSERT_TRUE(nfc.has_value());
    EXPECT_TRUE(index.lookup(remote(u"Music\\Album\\Cafe\u0301.mp3"_s)).has_value());
    EXPECT_TRUE(index.lookup(slsk::RemotePath{QByteArray("Music\\Album\\Caf\xe9.mp3")}).has_value());
    EXPECT_TRUE(index.lookup(slsk::RemotePath{QByteArray("music\\album\\CAF\xc9.MP3")}).has_value());
    // The local path keeps the bytes the file system gave.
    EXPECT_EQ(nfc->localPath.toUtf8(), nfdPath);
    EXPECT_EQ(nfc->entry.name.raw, u"Music\\Album\\Caf\u00e9.mp3"_s.toUtf8());
}

TEST(ShareLookupNames, AReplacedBackslashIsFoundUnderItsVirtualName)
{
    Tree tree;
    const QString local = tree.file(u"AC\\DC.mp3"_s);
    const BuildResult result = buildTree(tree);
    const std::optional<SharedFile> file = result.index->lookup(remote(u"Music\\AC\uff3cDC.mp3"_s));
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(QDir::cleanPath(file->localPath), QDir::cleanPath(local));
    // The backslash is a separator on the wire: this is the folder "AC" and the file "DC.mp3", which don't exist.
    EXPECT_FALSE(result.index->lookup(remote(u"Music\\AC\\DC.mp3"_s)).has_value());
}

TEST(ShareLookupNames, AliasesThatDifferOnlyInCaseStayApart)
{
    Tree tree;
    tree.file(u"A/one.mp3"_s, QByteArrayLiteral("1"));
    tree.file(u"B/one.mp3"_s, QByteArrayLiteral("22"));
    const BuildResult result =
        selecta::share::build({{tree.path(u"A"_s), u"Music"_s}, {tree.path(u"B"_s), u"MUSIC"_s}}, {});
    // Aliases are unique ignoring case ("Music", "MUSIC (2)"), so the case-insensitive fallback can't mix roots up.
    const std::optional<SharedFile> second = result.index->lookup(remote(u"music (2)\\ONE.MP3"_s));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->size, 2U);
    const std::optional<SharedFile> first = result.index->lookup(remote(u"MUSIC\\ONE.MP3"_s));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->size, 1U);
}

// --- folderContents() ------------------------------------------------------------------------------------------------

TEST_F(ShareLookup, ListsOneFolderWithoutSubfolders)
{
    const QList<slsk::SharedDirectory> folders = index().folderContents(remote(u"Music\\Artist\\Album"_s));
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(folders.at(0).path.display(), u"Music\\Artist\\Album"_s);
    ASSERT_EQ(folders.at(0).files.size(), 2);
    EXPECT_EQ(folders.at(0).files.at(0).name.display(), u"01 Track.mp3"_s); // bare names
    EXPECT_EQ(folders.at(0).files.at(0).size, 11U);
    EXPECT_EQ(folders.at(0).files.at(1).name.display(), u"cover.jpg"_s);
}

TEST_F(ShareLookup, AFolderWithOnlySubfoldersIsListedEmpty)
{
    const QList<slsk::SharedDirectory> folders = index().folderContents(remote(u"Music\\Artist"_s));
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(folders.at(0).path.display(), u"Music\\Artist"_s);
    EXPECT_TRUE(folders.at(0).files.isEmpty());
}

TEST_F(ShareLookup, FolderPathsIgnoreCaseAndAnswerWithTheIndexsSpelling)
{
    const QList<slsk::SharedDirectory> folders = index().folderContents(remote(u"MUSIC\\artist\\ALBUM"_s));
    ASSERT_EQ(folders.size(), 1);
    EXPECT_EQ(folders.at(0).path.display(), u"Music\\Artist\\Album"_s);
}

TEST_F(ShareLookup, UnknownFoldersListNothing)
{
    for (const QString &path : {u"Music\\Artist\\Nope"_s, u"Music\\Artist\\Album\\"_s, u"Music\\Artist\\Album\\01 Track.mp3"_s,
                                u"Nope"_s, QString(), m_tree.root()}) {
        EXPECT_TRUE(index().folderContents(remote(path)).isEmpty()) << path.toStdString();
    }
}

TEST_F(ShareLookup, FolderContentsSurviveSlsksOwnEncoding)
{
    const QList<slsk::SharedDirectory> folders = index().folderContents(remote(u"Music\\Artist\\Album"_s));
    slsk::peer::FolderContentsResponse response;
    response.token = 7;
    response.folder = remote(u"Music\\Artist\\Album"_s);
    response.directories = folders;
    const std::optional<slsk::peer::FolderContentsResponse> decoded =
        slsk::peer::FolderContentsResponse::decode(response.encode());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, response);
}

TEST_F(ShareLookup, ListingsMatchTheShareList)
{
    const slsk::SharedFileList list = test::decodedList(index());
    for (const slsk::SharedDirectory &directory : list.directories) {
        const QList<slsk::SharedDirectory> listed = index().folderContents(directory.path);
        ASSERT_EQ(listed.size(), 1);
        EXPECT_EQ(listed.at(0), directory);
        for (const slsk::FileEntry &file : directory.files) {
            const std::optional<SharedFile> found =
                index().lookup(remote(directory.path.display() + u'\\' + file.name.display()));
            ASSERT_TRUE(found.has_value());
            EXPECT_EQ(found->size, file.size);
            EXPECT_EQ(found->entry.attributes, file.attributes);
        }
    }
}

} // namespace
