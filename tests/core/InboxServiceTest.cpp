// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxService.h"

#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "library/Quarantine.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::InboxEntry;
using selecta::core::InboxService;
using selecta::core::LibraryConfig;
using selecta::library::HistoryEntry;
using selecta::library::InboxItem;

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

class InboxServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        config.inboxRoot = dir.filePath(u"Inbox"_s);
        config.collectionRoots = {dir.filePath(u"Music"_s)};
        ASSERT_TRUE(QDir().mkpath(config.inboxRoot) && QDir().mkpath(config.collectionRoots.first()));
        service = std::make_unique<InboxService>(db, config, [this](const QString &path, QString *trashedPath) {
            const QString target = dir.filePath(u"Trash/"_s) + QFileInfo(path).fileName();
            QDir().mkpath(QFileInfo(target).absolutePath());
            *trashedPath = target;
            return QFile::rename(path, target);
        });
    }

    // Puts a file in the Inbox the way the arrival pipeline does, with a history entry pointing at it.
    qint64 arrive(const QString &relativePath, InboxItem::Kind kind = InboxItem::Kind::Single)
    {
        const QString path = QDir(config.inboxRoot).filePath(relativePath);
        if (kind == InboxItem::Kind::Album) {
            EXPECT_TRUE(writeFile(path + u"/01 - One.mp3"_s, "one") && writeFile(path + u"/02 - Two.mp3"_s, "two"));
        } else {
            EXPECT_TRUE(writeFile(path, "audio"));
        }
        InboxItem item;
        item.kind = kind;
        item.relativePath = relativePath;
        item.state = InboxItem::State::Ready;
        item.createdAt = 1000;
        const auto id = inbox.add(item);
        EXPECT_TRUE(id.has_value());
        HistoryEntry entry;
        entry.completedAt = 1000;
        entry.peer = u"vinyl_ghost"_s;
        entry.remotePath = relativePath.toUtf8();
        entry.inboxItem = id;
        EXPECT_TRUE(history.add(entry).has_value());
        return id.value_or(0);
    }

    HistoryEntry::Whereabouts whereabouts()
    {
        const auto entries = history.recent(1);
        return entries.isEmpty() ? HistoryEntry::Whereabouts::Inbox : entries.first().whereabouts;
    }

    QTemporaryDir dir;
    selecta::storage::Database db;
    selecta::library::InboxStore inbox{db};
    selecta::library::HistoryStore history{db};
    LibraryConfig config;
    std::unique_ptr<InboxService> service;
};

} // namespace

TEST_F(InboxServiceTest, FilesASingleIntoTheSinglesFolder)
{
    const qint64 id = arrive(u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    EXPECT_TRUE(service->entries().constFirst().busy);
    service->waitForIdle();

    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    const QString filed = config.collectionRoots.first() + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s;
    EXPECT_TRUE(QFileInfo::exists(filed));
    EXPECT_EQ(inbox.count(), 0);
    EXPECT_EQ(whereabouts(), HistoryEntry::Whereabouts::Filed);
    EXPECT_EQ(history.recent(1).constFirst().filedPath, QFileInfo(filed).canonicalFilePath());
}

TEST_F(InboxServiceTest, FilesAnAlbumIntoTheAlbumsFolder)
{
    const qint64 id = arrive(u"Albums/Okuma Drift - Nightfall (2024)"_s, InboxItem::Kind::Album);
    service->file({id});
    service->waitForIdle();
    EXPECT_TRUE(
        QFileInfo::exists(config.collectionRoots.first() + u"/Albums/Okuma Drift - Nightfall (2024)/02 - Two.mp3"_s));
    EXPECT_EQ(inbox.count(), 0);
}

TEST_F(InboxServiceTest, QuarantinedItemsAreFiledWithTheirAttribute)
{
    if (!selecta::library::quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    // As arrivals leave them: quarantined singles and albums (NFR-4). Filing only moves them (DL-6).
    const qint64 single = arrive(u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s);
    const qint64 album = arrive(u"Albums/Okuma Drift - Nightfall (2024)"_s, InboxItem::Kind::Album);
    ASSERT_TRUE(selecta::library::quarantine::mark(config.inboxRoot + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s));
    ASSERT_TRUE(selecta::library::quarantine::markTree(config.inboxRoot + u"/Albums/Okuma Drift - Nightfall (2024)"_s));
    service->file({single, album});
    service->waitForIdle();
    EXPECT_EQ(inbox.count(), 0);
    const QString music = config.collectionRoots.first();
    const QStringList filedPaths{music + u"/Singles/Okuma_Drift_-_Neon_Rain.mp3"_s,
                                 music + u"/Albums/Okuma Drift - Nightfall (2024)"_s,
                                 music + u"/Albums/Okuma Drift - Nightfall (2024)/01 - One.mp3"_s};
    for (const QString &filed : filedPaths) {
        EXPECT_EQ(selecta::library::quarantine::agentOf(filed), u"Selecta"_s) << filed.toStdString();
    }
}

TEST_F(InboxServiceTest, ACollisionKeepsTheItemAndSaysWhy)
{
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    ASSERT_TRUE(writeFile(config.collectionRoots.first() + u"/Singles/Song.mp3"_s, "precious"));
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"already exists"_s));
    EXPECT_EQ(inbox.count(), 1);
    EXPECT_FALSE(service->entries().constFirst().busy);
}

TEST_F(InboxServiceTest, NoCollectionFolderMeansNothingMoves)
{
    config.collectionRoots.clear();
    service->setConfig(config);
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_EQ(inbox.count(), 1);
}

TEST_F(InboxServiceTest, TrashesAndRecordsIt)
{
    const qint64 id = arrive(u"Singles/Unwanted.mp3"_s);
    service->trash({id});
    service->waitForIdle();
    EXPECT_FALSE(QFileInfo::exists(config.inboxRoot + u"/Singles/Unwanted.mp3"_s));
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(u"Trash/Unwanted.mp3"_s)));
    EXPECT_EQ(inbox.count(), 0);
    EXPECT_EQ(whereabouts(), HistoryEntry::Whereabouts::Trashed);
}

TEST_F(InboxServiceTest, ATrashedItemCanBePutBack)
{
    const qint64 id = arrive(u"Albums/Okuma Drift - Night Signals"_s, InboxItem::Kind::Album);
    const QSignalSpy trashed(service.get(), &InboxService::trashed);
    service->trash({id});
    service->waitForIdle();
    ASSERT_EQ(trashed.size(), 1);
    const auto item = trashed.at(0).at(0).value<selecta::core::TrashedItem>();
    EXPECT_EQ(item.item.id, id);
    EXPECT_EQ(item.trashedPath, dir.filePath(u"Trash/Okuma Drift - Night Signals"_s));
    EXPECT_EQ(item.history.size(), 1);

    // Undo: the folder is back under its name, listed again, and its history says so.
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->restore(item);
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(config.inboxRoot + u"/Albums/Okuma Drift - Night Signals/01 - One.mp3"_s));
    ASSERT_EQ(inbox.count(), 1);
    const qint64 restored = finished.at(0).at(0).toLongLong();
    EXPECT_EQ(inbox.find(restored)->relativePath, u"Albums/Okuma Drift - Night Signals"_s);
    EXPECT_EQ(whereabouts(), HistoryEntry::Whereabouts::Inbox);
    EXPECT_EQ(history.recent(1).constFirst().inboxItem, restored);

    // Once it's gone from the Trash, there's nothing to put back.
    service->restore(item);
    service->waitForIdle();
    EXPECT_EQ(finished.size(), 2);
    EXPECT_FALSE(finished.at(1).at(1).toBool());
}

TEST_F(InboxServiceTest, ABusyItemIsNotStartedTwice)
{
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id, id});
    service->trash({id});
    service->waitForIdle();
    EXPECT_EQ(finished.size(), 1);
}

TEST_F(InboxServiceTest, ProcessingItemsWaitForTheirConversion)
{
    const qint64 id = arrive(u"Singles/Converting.mp3"_s);
    ASSERT_TRUE(inbox.setState(id, InboxItem::State::Processing));
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_EQ(inbox.count(), 1);
}

TEST_F(InboxServiceTest, ReconcileForgetsFilesDeletedInFinder)
{
    const qint64 kept = arrive(u"Singles/Kept.mp3"_s);
    arrive(u"Singles/Deleted.mp3"_s);
    ASSERT_TRUE(QFile::remove(config.inboxRoot + u"/Singles/Deleted.mp3"_s));
    const QSignalSpy changed(service.get(), &InboxService::changed);
    service->reconcile();
    ASSERT_EQ(changed.size(), 1);
    const auto entries = changed.at(0).at(0).value<QList<InboxEntry>>();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().item.id, kept);
}

TEST_F(InboxServiceTest, QuittingWithWorkInFlightStillRecordsIt)
{
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    service->file({id});
    service.reset(); // the destructor waits and records
    EXPECT_EQ(inbox.count(), 0);
    EXPECT_EQ(whereabouts(), HistoryEntry::Whereabouts::Filed);
}
