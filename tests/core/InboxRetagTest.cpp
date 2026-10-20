// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxRetag.h"

#include "SyntheticAudio.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"
#include "core/ArrivalService.h"
#include "core/InboxService.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::RetagRequest;
using selecta::core::RetagResult;
namespace keys = selecta::convert::keys;
namespace synth = selecta::testing;

namespace {

QByteArray jpeg(int side)
{
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG");
    return bytes;
}

class InboxRetagTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        inbox = QFileInfo(dir.path()).canonicalFilePath() + u"/Inbox"_s;
        ASSERT_TRUE(QDir().mkpath(inbox + u"/Singles"_s));
        path = inbox + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
        const QByteArray mp3 = synth::makeMp3(synth::sine(440.0, 2.0));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(mp3), mp3.size());
        file.close();
        selecta::convert::MetadataModel tags;
        tags.set(keys::Artist, u"Okuma Drift"_s, selecta::convert::Provenance::Release);
        tags.set(keys::Title, u"Neon Rain"_s, selecta::convert::Provenance::Release);
        tags.set(keys::Genre, u"Electronic"_s, selecta::convert::Provenance::Release);
        ASSERT_TRUE(selecta::convert::writeTags(path, tags).ok());
    }

    RetagRequest request() const
    {
        RetagRequest r;
        r.path = path;
        r.inboxRoot = inbox;
        return r;
    }

    QTemporaryDir dir;
    QString inbox;
    QString path;
};

} // namespace

TEST_F(InboxRetagTest, WritesTheNewTagsAndRenamesTheFile)
{
    const selecta::library::FileOps fileOps(inbox, {});
    RetagRequest r = request();
    r.changes.set(keys::Title, u"Neon Rain (Extended Mix)"_s, selecta::convert::Provenance::User);
    r.cleared = {keys::Genre.toString()};
    selecta::convert::Picture cover;
    cover.data = jpeg(1200); // bigger than CDJs like: processed down
    cover.provenance = selecta::convert::Provenance::Release;
    r.changes.setFrontCover(cover);

    const RetagResult result = selecta::core::retagInboxFile(r, fileOps);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    // Named by the template and the cleaner (DL-13), as arrivals are.
    const QString expected = selecta::library::naming::componentName(
        selecta::library::naming::Templates{}.single,
        selecta::core::namingFieldsOf(selecta::convert::readTags(result.path).value()),
        selecta::library::naming::Cleaner{}, u"mp3"_s);
    EXPECT_EQ(QFileInfo(result.path).fileName(), expected);
    EXPECT_TRUE(expected.contains(u"Extended"_s));
    EXPECT_EQ(result.sizeBytes, QFileInfo(result.path).size());
    const auto read = selecta::convert::readTags(result.path);
    ASSERT_TRUE(read);
    EXPECT_EQ(read.value().value(keys::Title), u"Neon Rain (Extended Mix)"_s);
    EXPECT_EQ(read.value().value(keys::Artist), u"Okuma Drift"_s); // kept
    EXPECT_FALSE(read.value().contains(keys::Genre));
    ASSERT_TRUE(read.value().frontCover().has_value());
    const QImage written = QImage::fromData(read.value().frontCover()->data);
    EXPECT_LE(written.width(), 800);
    // No copy left behind.
    EXPECT_EQ(QDir(inbox + u"/Singles"_s).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

TEST_F(InboxRetagTest, TheSameNameStaysPut)
{
    const selecta::library::FileOps fileOps(inbox, {});
    RetagRequest r = request();
    r.changes.set(keys::Album, u"Nightfall"_s, selecta::convert::Provenance::User);
    const RetagResult result = selecta::core::retagInboxFile(r, fileOps);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(QFileInfo(result.path).canonicalFilePath(), QFileInfo(path).canonicalFilePath());
    EXPECT_EQ(selecta::convert::readTags(path).value().value(keys::Album), u"Nightfall"_s);
}

TEST_F(InboxRetagTest, RefusesFilesOutsideTheInbox)
{
    const QString outside = dir.filePath(u"elsewhere.mp3"_s);
    ASSERT_TRUE(QFile::copy(path, outside));
    const selecta::library::FileOps fileOps(inbox, {});
    RetagRequest r = request();
    r.path = outside;
    r.changes.set(keys::Title, u"Changed"_s, selecta::convert::Provenance::User);
    const RetagResult result = selecta::core::retagInboxFile(r, fileOps);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(selecta::convert::readTags(outside).value().value(keys::Title), u"Neon Rain"_s);
}

TEST_F(InboxRetagTest, ABrokenCoverLeavesTheFileAsItWas)
{
    const selecta::library::FileOps fileOps(inbox, {});
    RetagRequest r = request();
    r.changes.set(keys::Title, u"Changed"_s, selecta::convert::Provenance::User);
    selecta::convert::Picture cover;
    cover.data = QByteArray("not an image");
    r.changes.setFrontCover(cover);
    const RetagResult result = selecta::core::retagInboxFile(r, fileOps);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_EQ(selecta::convert::readTags(path).value().value(keys::Title), u"Neon Rain"_s);
}

TEST_F(InboxRetagTest, TheInboxServiceUpdatesItsItem)
{
    selecta::storage::Database db;
    ASSERT_TRUE(db.open(u":memory:"_s));
    ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    selecta::core::LibraryConfig config;
    config.inboxRoot = inbox;
    selecta::core::InboxService service(db, config, [](const QString &, QString *) { return true; });
    selecta::library::InboxItem item;
    item.relativePath = u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
    item.state = selecta::library::InboxItem::State::Ready;
    item.artist = u"Okuma Drift"_s;
    item.title = u"Neon Rain"_s;
    item.format = u"MP3"_s;
    item.tagSource = u"spotify"_s;
    const std::optional<qint64> id = service.add(item);
    ASSERT_TRUE(id.has_value());

    const QSignalSpy finished(&service, &selecta::core::InboxService::finished);
    selecta::convert::MetadataModel changes;
    changes.set(keys::Artist, u"Okuma Drift, Halden Rue"_s, selecta::convert::Provenance::User);
    service.retag(*id, changes, {}, u"user"_s);
    service.waitForIdle();
    QCoreApplication::processEvents();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    const std::optional<selecta::library::InboxItem> updated = service.item(*id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->artist, u"Okuma Drift, Halden Rue"_s);
    EXPECT_EQ(updated->tagSource, u"user"_s);
    EXPECT_TRUE(updated->relativePath.startsWith(u"Singles/"_s));
    EXPECT_TRUE(updated->relativePath.contains(u"Halden"_s)) << updated->relativePath.toStdString();
    EXPECT_TRUE(QFileInfo::exists(service.absolutePath(*updated)));
}
