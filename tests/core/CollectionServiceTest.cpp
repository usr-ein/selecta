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
using selecta::core::CollectionService;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;

namespace {

bool writeTrack(const QString &path, const QString &artist, const QString &title, double seconds = 4.0)
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
        ASSERT_TRUE(writeTrack(root + u"/Singles/Okuma Drift - Neon Rain.mp3"_s, u"Okuma Drift"_s, u"Neon Rain"_s));
        ASSERT_TRUE(writeTrack(root + u"/Singles/Halden Rue - Tidewater.flac"_s, u"Halden Rue"_s, u"Tidewater"_s));
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

    QTemporaryDir dir;
    QString root;
    selecta::storage::Database db;
    std::unique_ptr<CollectionService> service;
};

} // namespace

TEST_F(CollectionServiceTest, ScansTheFoldersIntoTheIndex)
{
    const QSignalSpy indexes(service.get(), &CollectionService::indexChanged);
    service->setRoots({root}, {});
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
}

TEST_F(CollectionServiceTest, ARescanFollowsChangesAndDeletions)
{
    service->setRoots({root}, {});
    service->waitForIdle();
    ASSERT_TRUE(writeTrack(root + u"/New/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    ASSERT_TRUE(QFile::remove(root + u"/Singles/Halden Rue - Tidewater.flac"_s));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
    EXPECT_TRUE(lookUp(u"Kassia Moreau"_s, u"Night Transit"_s).has_value());
    EXPECT_FALSE(lookUp(u"Halden Rue"_s, u"Tidewater"_s).has_value());
}

TEST_F(CollectionServiceTest, AMissingFolderKeepsItsTracks)
{
    service->setRoots({root}, {});
    service->waitForIdle();
    ASSERT_TRUE(QDir(root).rename(root, root + u"-unplugged"_s));
    service->scanNow();
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2); // an unplugged drive isn't a deletion
    EXPECT_TRUE(service->status().message.contains(u"Not there"_s)) << service->status().message.toStdString();
    EXPECT_TRUE(lookUp(u"Okuma Drift"_s, u"Neon Rain"_s).has_value());
}

TEST_F(CollectionServiceTest, FoldersRemovedFromSettingsAreForgotten)
{
    service->setRoots({root}, {});
    service->waitForIdle();
    service->setRoots({}, {});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 0);
    EXPECT_EQ(selecta::core::CollectionStore(db).count(), 0);
    EXPECT_FALSE(lookUp(u"Okuma Drift"_s, u"Neon Rain"_s).has_value());
}

TEST_F(CollectionServiceTest, SelectasOwnFoldersInsideTheCollectionArentPartOfIt)
{
    const QString inbox = root + u"/Selecta Inbox"_s;
    ASSERT_TRUE(
        writeTrack(inbox + u"/Singles/Kassia Moreau - Night Transit.mp3"_s, u"Kassia Moreau"_s, u"Night Transit"_s));
    service->setRoots({root}, {inbox});
    service->waitForIdle();
    EXPECT_EQ(service->status().tracks, 2);
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
    service->setRoots({root}, {});
    service->setInbox({item});
    service->waitForIdle();
    const auto found = lookUp(u"Kassia Moreau"_s, u"Night Transit"_s);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->source, selecta::match::Source::Inbox);
    EXPECT_EQ(found->id, 7U);
}

TEST_F(CollectionServiceTest, GetTracksLinesYouHaveAreMarkedAndUnticked)
{
    service->setRoots({root}, {});
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
    ASSERT_TRUE(QFile::remove(root + u"/Singles/Okuma Drift - Neon Rain.mp3"_s));
    service->scanNow();
    service->waitForIdle();
    getTracks.setLibraryIndex(service->index());
    EXPECT_EQ(getTracks.lines().at(0).findState, selecta::core::GetLine::FindState::Idle);
}
