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
        config.tracksRoot = dir.filePath(u"Music/Tracks"_s);
        config.albumsRoot = dir.filePath(u"Music/Albums"_s);
        ASSERT_TRUE(QDir().mkpath(config.inboxRoot) && QDir().mkpath(config.tracksRoot) &&
                    QDir().mkpath(config.albumsRoot));
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

TEST_F(InboxServiceTest, FilesASingleIntoTheTracksFolder)
{
    const qint64 id = arrive(u"Singles/Okuma_Drift_-_Neon_Rain.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    EXPECT_TRUE(service->entries().constFirst().busy);
    service->waitForIdle();

    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    const QString filed =
        config.tracksRoot + u"/Okuma_Drift_-_Neon_Rain.mp3"_s; // loose, as the tracks folder holds them
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
    EXPECT_TRUE(QFileInfo::exists(config.albumsRoot + u"/Okuma Drift - Nightfall (2024)/02 - Two.mp3"_s));
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
    const QStringList filedPaths{config.tracksRoot + u"/Okuma_Drift_-_Neon_Rain.mp3"_s,
                                 config.albumsRoot + u"/Okuma Drift - Nightfall (2024)"_s,
                                 config.albumsRoot + u"/Okuma Drift - Nightfall (2024)/01 - One.mp3"_s};
    for (const QString &filed : filedPaths) {
        EXPECT_EQ(selecta::library::quarantine::agentOf(filed), u"Selecta"_s) << filed.toStdString();
    }
}

TEST_F(InboxServiceTest, ACollisionKeepsTheItemAndSaysWhy)
{
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    ASSERT_TRUE(writeFile(config.tracksRoot + u"/Song.mp3"_s, "precious"));
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
    config.tracksRoot.clear();
    config.albumsRoot.clear();
    service->setConfig(config);
    const qint64 id = arrive(u"Singles/Song.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_EQ(finished.at(0).at(2).toString(), u"Choose your tracks folder in Settings first."_s);
    EXPECT_EQ(inbox.count(), 1);
}

TEST_F(InboxServiceTest, EachKindNeedsItsOwnFolder)
{
    // Only the tracks folder chosen: singles are filed, albums wait for their folder.
    config.albumsRoot.clear();
    service->setConfig(config);
    const qint64 single = arrive(u"Singles/Song.mp3"_s);
    const qint64 album = arrive(u"Albums/Okuma Drift - Nightfall (2024)"_s, InboxItem::Kind::Album);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({single, album});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 2);
    EXPECT_EQ(finished.at(0).at(0).toLongLong(), album); // refused at once, before the single's move finished
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_EQ(finished.at(0).at(2).toString(), u"Choose your albums folder in Settings first."_s);
    EXPECT_TRUE(finished.at(1).at(1).toBool()) << finished.at(1).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Song.mp3"_s));
    ASSERT_EQ(inbox.count(), 1);
    EXPECT_EQ(inbox.all().constFirst().id, album);
}

TEST(LibraryConfigCheck, TheTwoFoldersStayApartFromEachOtherAndFromTheInbox)
{
    using selecta::core::checkLibraryConfig;
    LibraryConfig config;
    config.inboxRoot = u"/Data/Selecta/Inbox"_s;
    EXPECT_EQ(checkLibraryConfig(config), QString()); // nothing chosen yet: Settings asks, nothing is wrong
    config.tracksRoot = u"/Music/Tracks"_s;
    EXPECT_EQ(checkLibraryConfig(config), QString());
    config.albumsRoot = u"/Music/Albums"_s;
    EXPECT_EQ(checkLibraryConfig(config), QString());
    config.albumsRoot = u"/Music/Tracks2"_s; // a name prefix isn't nesting
    EXPECT_EQ(checkLibraryConfig(config), QString());

    const auto refused = [&](const QString &tracks, const QString &albums) {
        LibraryConfig changed = config;
        changed.tracksRoot = tracks;
        changed.albumsRoot = albums;
        return checkLibraryConfig(changed);
    };
    EXPECT_EQ(refused(u"/Music"_s, u"/Music/"_s),
              u"The tracks folder and the albums folder must be two different folders."_s);
    EXPECT_EQ(refused(u"/Music/Bj\u00F6rk"_s, u"/Music/Bjo\u0308rk"_s), // NFC and NFD spellings of one folder
              u"The tracks folder and the albums folder must be two different folders."_s);
    EXPECT_EQ(refused(u"/Music"_s, u"/Music/Albums"_s),
              u"The tracks folder and the albums folder can't be inside one another."_s);
    EXPECT_EQ(refused(u"/Music/Albums/Tracks"_s, u"/Music/Albums"_s),
              u"The tracks folder and the albums folder can't be inside one another."_s);
    EXPECT_FALSE(refused(u"Music/Tracks"_s, {}).isEmpty()); // not a full path
    EXPECT_FALSE(refused({}, u"Albums"_s).isEmpty());
    // Selecta's own Inbox: neither inside a collection folder nor holding one.
    EXPECT_FALSE(refused(u"/Data/Selecta/Inbox/Singles"_s, {}).isEmpty());
    EXPECT_FALSE(refused({}, u"/Data/Selecta/Inbox"_s).isEmpty());
    EXPECT_FALSE(refused(u"/Data"_s, {}).isEmpty());
    EXPECT_FALSE(refused({}, u"/"_s).isEmpty());
    EXPECT_EQ(refused(u"/Data/Selecta/Inbox2"_s, {}), QString());

    LibraryConfig noInbox = config;
    noInbox.inboxRoot.clear();
    EXPECT_FALSE(checkLibraryConfig(noInbox).isEmpty());
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

// P-001 (PRD D39): after the update, an Inbox item recorded by an earlier build leaves the list without a word in
// History. History says "In the Inbox" as before, never "Moved to the Trash": the file was never trashed.
TEST(InboxAfterTheMove, TheOldItemsLeaveAndHistoryStaysAsItWas)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    selecta::storage::Database db;
    ASSERT_TRUE(db.open(dir.filePath(u"selecta.sqlite3"_s)));
    const auto all = selecta::storage::selectaMigrations();
    const auto beforeTheMove = static_cast<std::size_t>(selecta::storage::kInboxMovedVersion - 1);
    ASSERT_TRUE(selecta::storage::migrate(db, all.first(beforeTheMove)).ok()); // the schema of earlier builds

    // Before the update: one single in an earlier build's Inbox, with its History entry, as that build wrote them
    // (the stores write the newest schema).
    const QString oldFile = dir.filePath(u"Music/Selecta/Inbox/Singles/Okuma Drift - Neon Rain.mp3"_s);
    ASSERT_TRUE(writeFile(oldFile, "audio"));
    ASSERT_TRUE(db.exec(uR"sql(
        INSERT INTO inbox_items (id, kind, relative_path, state, created_at)
        VALUES (1, 'single', 'Singles/Okuma Drift - Neon Rain.mp3', 'ready', 1000);
        INSERT INTO history (completed_at, kind, peer, remote_path, size_bytes, inbox_item, whereabouts)
        VALUES (1000, 'single', 'vinyl_ghost', CAST('Singles/Okuma Drift - Neon Rain.mp3' AS BLOB), 0, 1, 'inbox');
    )sql"_s))
        << db.lastError().toStdString();

    // The update: the database's migrations, then the Inbox in Selecta's data folder.
    ASSERT_TRUE(selecta::storage::migrate(db, all).ok());
    LibraryConfig config;
    config.inboxRoot = dir.filePath(u"data/Inbox"_s);
    ASSERT_TRUE(QDir().mkpath(config.inboxRoot));
    InboxService service(db, config, [](const QString &, QString *) { return false; });
    service.reconcile();

    EXPECT_TRUE(service.entries().isEmpty());
    const QList<HistoryEntry> after = selecta::library::HistoryStore(db).recent(10);
    ASSERT_EQ(after.size(), 1);
    EXPECT_EQ(after.first().whereabouts, HistoryEntry::Whereabouts::Inbox);
    EXPECT_FALSE(after.first().inboxItem.has_value());
    EXPECT_TRUE(QFileInfo::exists(oldFile)); // Selecta never touches it
}

// --- Upgrades (P-010, PRD D41) ---------------------------------------------------------------------------------------

namespace {

void markUpgrade(selecta::library::InboxStore &inbox, qint64 id, const QString &replaces)
{
    std::optional<InboxItem> item = inbox.find(id);
    ASSERT_TRUE(item.has_value());
    item->replaces = replaces;
    ASSERT_TRUE(inbox.update(*item));
}

} // namespace

TEST_F(InboxServiceTest, AnUpgradeTakesItsFilesPlaceAndTheOldOneGoesToTheTrash)
{
    // The old file, downloaded and filed once before.
    const QString old = config.albumsRoot + u"/Okuma Drift - Nightfall/01 Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old 128"));
    HistoryEntry before;
    before.completedAt = 500;
    before.peer = u"bassbin"_s;
    before.remotePath = "@@b\\Dusk.mp3";
    before.whereabouts = HistoryEntry::Whereabouts::Filed;
    before.filedPath = QFileInfo(old).canonicalFilePath();
    const auto beforeId = history.add(before);
    ASSERT_TRUE(beforeId.has_value());

    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, old);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();

    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(finished.at(0).at(2).toString().startsWith(u"Replaced "_s)) << finished.at(0).at(2).toString().toStdString();
    QFile replaced(old);
    ASSERT_TRUE(replaced.open(QIODevice::ReadOnly));
    EXPECT_EQ(replaced.readAll(), "audio"); // the upgrade, at the exact path
    EXPECT_FALSE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s)); // not a new file
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(u"Trash/01 Dusk.mp3"_s)));
    EXPECT_EQ(inbox.count(), 0);
    // History: the upgrade is filed there, the file it replaced went to the Trash.
    EXPECT_EQ(history.recent(1).constFirst().whereabouts, HistoryEntry::Whereabouts::Filed);
    EXPECT_EQ(history.recent(1).constFirst().filedPath, QFileInfo(old).canonicalFilePath());
    EXPECT_EQ(history.find(*beforeId)->whereabouts, HistoryEntry::Whereabouts::Trashed);
}

TEST_F(InboxServiceTest, AnUpgradeOfAnotherTypeIsFiledAsANewFileAndSaysWhy)
{
    const QString old = config.tracksRoot + u"/Okuma Drift - Dusk.m4a"_s;
    ASSERT_TRUE(writeFile(old, "aac"));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, old);
    const std::optional<InboxItem> item = inbox.find(id);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(selecta::core::upgradeMismatch(*item),
              u"The file it would replace is of another type (M4A, not MP3): Save to collection adds this one as a new "
              "file."_s);

    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool());
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"couldn't replace"_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(old)); // untouched
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));
}

TEST_F(InboxServiceTest, AnUpgradeWhoseFolderWentIsFiledAsANewFile)
{
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, config.albumsRoot + u"/Gone/01 Dusk.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s));
}

// An upgrade needs no tracks folder to take an album track's place; filed as a new file, it would.
TEST_F(InboxServiceTest, AnUpgradeNeedsTheTracksFolderOnlyAsANewFile)
{
    config.tracksRoot.clear();
    service->setConfig(config);
    const QString old = config.albumsRoot + u"/Okuma Drift - Nightfall/01 Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old"));
    const qint64 replacing = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, replacing, old);
    const qint64 mismatched = arrive(u"Singles/Okuma Drift - Dawn.mp3"_s);
    markUpgrade(inbox, mismatched, config.albumsRoot + u"/Okuma Drift - Nightfall/02 Dawn.flac"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({replacing, mismatched});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 2);
    for (const QList<QVariant> &result : finished) {
        const bool ok = result.at(1).toBool();
        EXPECT_EQ(ok, result.at(0).toLongLong() == replacing) << result.at(2).toString().toStdString();
    }
    EXPECT_EQ(inbox.count(), 1); // the mismatched one waits for a tracks folder
}
