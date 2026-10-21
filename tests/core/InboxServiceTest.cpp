// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InboxService.h"

#include "collection/FolderRoles.h"
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

using selecta::core::QualityTier;
using selecta::core::TierInCollection;
using selecta::core::UpgradeVerdict;

// Marks an Inbox item an upgrade of `replaces`, downloaded as `source` ("FLAC 16/44.1", "MP3 192"), the spectrum
// check having found it made from a lossy file of `likely` kbps.
void markUpgrade(selecta::library::InboxStore &inbox, qint64 id, const QString &replaces,
                 const QString &source = u"FLAC 16/44.1"_s, std::optional<int> likely = std::nullopt)
{
    std::optional<InboxItem> item = inbox.find(id);
    ASSERT_TRUE(item.has_value());
    item->replaces = replaces;
    item->format = u"MP3"_s;
    item->bitrate = 320;
    item->sourceFormat = source;
    item->qualitySourceKbps = likely;
    ASSERT_TRUE(inbox.update(*item));
}

// The collection, as far as the Inbox asks: every file at `tier`.
selecta::core::TierLookup collectionAt(QualityTier tier)
{
    return [tier](const QString &) { return TierInCollection::of(tier); };
}

// The collection, read, without the file asked about; or not read yet.
TierInCollection notInCollection(const QString & /*path*/)
{
    return TierInCollection::notInCollection();
}
TierInCollection notReadYet(const QString & /*path*/)
{
    return {};
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

TEST(UpgradeJudgement, TheRealTierAgainstTheFilesNow)
{
    using selecta::core::judgeUpgrade;
    const selecta::collection::FolderRoles folders(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    InboxItem item;
    item.kind = InboxItem::Kind::Single;
    item.relativePath = u"Singles/Okuma Drift - Dusk.mp3"_s;
    item.format = u"MP3"_s;
    item.bitrate = 320;
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::From256), folders).verdict, UpgradeVerdict::None);
    item.replaces = u"/Music/Tracks/Okuma Drift - Dusk.mp3"_s;

    // From a FLAC: better than a 256.
    item.sourceFormat = u"FLAC 16/44.1"_s;
    EXPECT_EQ(selecta::core::downloadTier(item), QualityTier::Lossless);
    auto judged = judgeUpgrade(item, collectionAt(QualityTier::From256), folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::Replaces);
    EXPECT_EQ(judged.note, u"Better (From lossless; the file: 256–319 kbps): Save to collection puts it in that file's "
                           "place, under the same name, and moves that file to the Trash."_s);
    // A FLAC the spectrum check found made from a 128 kbps MP3: not better.
    item.qualitySourceKbps = 128;
    EXPECT_EQ(selecta::core::downloadTier(item), QualityTier::Below192);
    judged = judgeUpgrade(item, collectionAt(QualityTier::From256), folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::NotBetter);
    EXPECT_EQ(judged.note, u"Not better (Below 192 kbps; the file: 256–319 kbps): Save to collection adds it as a new "
                           "file and leaves that file as it is."_s);
    // A peer that said 320 and sent 192: the MP3 is kept as it came.
    item.qualitySourceKbps.reset();
    item.sourceFormat = u"MP3 192"_s;
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::From256), folders).verdict, UpgradeVerdict::NotBetter);
    // The same tier isn't better; a V0 beats a 256; nothing beats a file of unknown quality but a known one.
    item.sourceFormat = u"MP3 320"_s;
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::Top), folders).verdict, UpgradeVerdict::NotBetter);
    item.sourceFormat = u"MP3 VBR 245"_s;
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::From256), folders).verdict, UpgradeVerdict::Replaces);
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::Unknown), folders).verdict, UpgradeVerdict::Replaces);

    // A file the collection hasn't any more leaves its place free while its folder is the collection's.
    judged = judgeUpgrade(item, notInCollection, folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::Replaces);
    EXPECT_EQ(judged.note,
              u"The file isn't in your collection any more: Save to collection puts this one in its place."_s);
    // Not when its folder left the collection (other folders chosen since; a subfolder of the tracks folder).
    for (const QString &outside : {u"/Elsewhere/Okuma Drift - Dusk.mp3"_s, u"/Music/Tracks/Old/Okuma Drift - Dusk.mp3"_s}) {
        item.replaces = outside;
        judged = judgeUpgrade(item, notInCollection, folders);
        EXPECT_EQ(judged.verdict, UpgradeVerdict::CantReplace) << outside.toStdString();
        EXPECT_EQ(judged.note, u"The file isn't in your collection folders any more: Save to collection adds this one "
                               "as a new file."_s);
    }
    // Before the collection is read (or without a lookup), nothing is known: nothing is replaced, saving waits.
    item.replaces = u"/Music/Tracks/Okuma Drift - Dusk.mp3"_s;
    judged = judgeUpgrade(item, notReadYet, folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::NotKnownYet);
    EXPECT_EQ(judged.note,
              u"Selecta is still reading your collection: in a moment, it tells whether this one is better."_s);
    EXPECT_EQ(judgeUpgrade(item, {}, folders).verdict, UpgradeVerdict::NotKnownYet);

    // An AAC, Ogg or Opus file's upgrade takes its name with .mp3 (the maintainer: "Allow .mp3 for them").
    item.replaces = u"/Music/Albums/Nightfall/01 Dusk.m4a"_s;
    judged = judgeUpgrade(item, collectionAt(QualityTier::Below192), folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::Replaces);
    EXPECT_EQ(judged.note, u"Better (320 kbps or V0; the file: Below 192 kbps): Save to collection puts it in that "
                           "file's place, as 01 Dusk.mp3, and moves that file to the Trash."_s);
    judged = judgeUpgrade(item, notInCollection, folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::Replaces);
    EXPECT_EQ(judged.note, u"The file isn't in your collection any more: Save to collection puts this one in its "
                           "place, as 01 Dusk.mp3."_s);
    // Any other type can't keep the name, whatever the collection says.
    item.replaces = u"/Music/Tracks/Okuma Drift - Dusk.flac"_s;
    judged = judgeUpgrade(item, collectionAt(QualityTier::Below192), folders);
    EXPECT_EQ(judged.verdict, UpgradeVerdict::CantReplace);
    EXPECT_EQ(judged.note, u"The file it would replace is of another type (FLAC, not MP3): Save to collection adds this "
                           "one as a new file."_s);
    EXPECT_EQ(judgeUpgrade(item, notReadYet, folders).verdict, UpgradeVerdict::CantReplace);
    // An album never takes a track's place.
    item.replaces = u"/Music/Tracks/Okuma Drift - Dusk.mp3"_s;
    item.kind = InboxItem::Kind::Album;
    EXPECT_EQ(judgeUpgrade(item, collectionAt(QualityTier::Below192), folders).verdict, UpgradeVerdict::CantReplace);
    // No source quality kept (an earlier item): as it is now.
    item.kind = InboxItem::Kind::Single;
    item.sourceFormat.clear();
    EXPECT_EQ(selecta::core::downloadTier(item), QualityTier::Top);
}

TEST_F(InboxServiceTest, AnUpgradeTakesItsFilesPlaceAndTheOldOneGoesToTheTrash)
{
    service->setTierLookup(collectionAt(QualityTier::Below192));
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
    service->refresh();
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::Replaces);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();

    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(finished.at(0).at(2).toString().startsWith(u"Replaced "_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_EQ(contents(old), "audio");                                                 // the upgrade, at the exact path
    EXPECT_FALSE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s)); // not a new file
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(u"Trash/01 Dusk.mp3"_s)));
    EXPECT_EQ(inbox.count(), 0);
    // History: the upgrade is filed there, the file it replaced went to the Trash.
    EXPECT_EQ(history.recent(1).constFirst().whereabouts, HistoryEntry::Whereabouts::Filed);
    EXPECT_EQ(history.recent(1).constFirst().filedPath, QFileInfo(old).canonicalFilePath());
    EXPECT_EQ(history.find(*beforeId)->whereabouts, HistoryEntry::Whereabouts::Trashed);
}

// Review round 1: what arrives must really be better. A FLAC the spectrum check found made from a 128 kbps MP3, and an
// MP3 its peer said was 320 that came at 192, never take a 256's place: each is filed as a new file, the 256 stays.
TEST_F(InboxServiceTest, AnUpgradeThatIsntBetterNeverReplacesItsFile)
{
    service->setTierLookup(collectionAt(QualityTier::From256));
    const QString fake = config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s;
    const QString lied = config.tracksRoot + u"/Okuma Drift - Dawn.mp3"_s;
    ASSERT_TRUE(writeFile(fake, "real 256") && writeFile(lied, "real 256 too"));
    const qint64 fromFake = arrive(u"Singles/Okuma Drift - Dusk (2).mp3"_s);
    markUpgrade(inbox, fromFake, fake, u"FLAC 16/44.1"_s, 128);
    const qint64 fromLiar = arrive(u"Singles/Okuma Drift - Dawn (2).mp3"_s);
    markUpgrade(inbox, fromLiar, lied, u"MP3 192"_s);
    service->refresh();
    const QList<selecta::core::InboxEntry> entries = service->entries();
    ASSERT_EQ(entries.size(), 2);
    for (const selecta::core::InboxEntry &entry : entries) {
        EXPECT_EQ(entry.upgrade, UpgradeVerdict::NotBetter) << entry.item.relativePath.toStdString();
        EXPECT_TRUE(entry.upgradeNote.startsWith(u"Not better"_s)) << entry.upgradeNote.toStdString();
    }

    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({fromFake, fromLiar});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 2);
    for (const QList<QVariant> &result : finished) {
        EXPECT_TRUE(result.at(1).toBool());
        EXPECT_TRUE(result.at(2).toString().contains(u"as a new file"_s)) << result.at(2).toString().toStdString();
    }
    EXPECT_EQ(contents(fake), "real 256"); // untouched
    EXPECT_EQ(contents(lied), "real 256 too");
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk (2).mp3"_s));
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dawn (2).mp3"_s));
}

// Judged again when filed: the file may have got better since the list said "Upgrade".
TEST_F(InboxServiceTest, AnUpgradeIsJudgedAgainWhenFiled)
{
    QualityTier now = QualityTier::Below192;
    service->setTierLookup([&now](const QString &) { return TierInCollection::of(now); });
    const QString old = config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old"));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk (2).mp3"_s);
    markUpgrade(inbox, id, old, u"MP3 320"_s);
    service->refresh();
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::Replaces);
    now = QualityTier::Lossless; // replaced by hand with a better one meanwhile
    service->file({id});
    service->waitForIdle();
    EXPECT_EQ(contents(old), "old");
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk (2).mp3"_s));
}

// The maintainer: "Allow .mp3 for them". An AAC file's upgrade keeps its folder and name, with .mp3; the AAC goes to
// the Trash, and History says so.
TEST_F(InboxServiceTest, AnAacFilesUpgradeTakesItsNameWithMp3)
{
    service->setTierLookup(collectionAt(QualityTier::Below192));
    const QString old = config.albumsRoot + u"/Okuma Drift - Nightfall/03 Night Bus.m4a"_s;
    ASSERT_TRUE(writeFile(old, "aac 128"));
    HistoryEntry before;
    before.completedAt = 500;
    before.peer = u"bassbin"_s;
    before.remotePath = "@@b\\Night Bus.m4a";
    before.whereabouts = HistoryEntry::Whereabouts::Filed;
    before.filedPath = QFileInfo(old).canonicalFilePath();
    const auto beforeId = history.add(before);
    ASSERT_TRUE(beforeId.has_value());
    const qint64 id = arrive(u"Singles/Ruben Ostrava - Night Bus.mp3"_s);
    markUpgrade(inbox, id, old);
    service->refresh();
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::Replaces);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    const QString message = finished.at(0).at(2).toString();
    EXPECT_TRUE(message.startsWith(u"Replaced "_s) && message.contains(u"03 Night Bus.m4a with "_s))
        << message.toStdString();
    const QString mp3 = config.albumsRoot + u"/Okuma Drift - Nightfall/03 Night Bus.mp3"_s;
    EXPECT_EQ(contents(mp3), "audio");
    EXPECT_FALSE(QFileInfo::exists(old));
    EXPECT_TRUE(QFileInfo::exists(dir.filePath(u"Trash/03 Night Bus.m4a"_s)));
    EXPECT_EQ(inbox.count(), 0);
    EXPECT_EQ(history.find(*beforeId)->whereabouts, HistoryEntry::Whereabouts::Trashed);
    EXPECT_EQ(history.recent(1).constFirst().filedPath, QFileInfo(mp3).canonicalFilePath());
}

TEST_F(InboxServiceTest, AnUpgradeOfAnotherTypeIsFiledAsANewFileAndSaysWhy)
{
    service->setTierLookup(collectionAt(QualityTier::Below192));
    const QString old = config.tracksRoot + u"/Okuma Drift - Dusk.flac"_s;
    ASSERT_TRUE(writeFile(old, "flac"));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, old);
    service->refresh();
    EXPECT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::CantReplace);

    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool());
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"stays as it was"_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(QFileInfo::exists(old)); // untouched
    EXPECT_TRUE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));
}

// Review round 1: a folder that isn't there (an unplugged drive) keeps the upgrade waiting, never a stray new file.
TEST_F(InboxServiceTest, AnUpgradeWhoseFolderIsntThereWaits)
{
    service->setTierLookup(collectionAt(QualityTier::Below192));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, config.albumsRoot + u"/Gone/01 Dusk.mp3"_s);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"plug its drive in"_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_EQ(inbox.count(), 1);
    EXPECT_FALSE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s));
}

// Review round 1: just after launch the collection isn't read yet, so nothing tells whether an upgrade is better.
// Saving it waits, and touches nothing; once the collection is read, it is judged.
TEST_F(InboxServiceTest, AnUpgradeWaitsUntilTheCollectionIsRead)
{
    const QString old = config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s;
    ASSERT_TRUE(writeFile(old, "old"));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk (2).mp3"_s);
    markUpgrade(inbox, id, old);
    service->setTierLookup(notReadYet);
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::NotKnownYet);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_FALSE(finished.at(0).at(1).toBool());
    EXPECT_EQ(finished.at(0).at(2).toString(), u"Selecta is still reading your collection, to tell whether Okuma Drift "
                                                "- Dusk (2).mp3 is better: try again in a moment."_s);
    EXPECT_EQ(contents(old), "old");
    EXPECT_EQ(inbox.count(), 1);
    EXPECT_FALSE(QFileInfo::exists(config.tracksRoot + u"/Okuma Drift - Dusk (2).mp3"_s));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));

    const QSignalSpy changed(service.get(), &InboxService::changed);
    service->setTierLookup(collectionAt(QualityTier::Below192));
    ASSERT_EQ(changed.size(), 1); // the list again, judged
    const auto entries = changed.at(0).at(0).value<QList<InboxEntry>>();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.constFirst().upgrade, UpgradeVerdict::Replaces);
}

// A file deleted since, or a replace a crash cut short (the old file in the Trash, the upgrade still here): the place
// is free, and the upgrade takes it. Nothing goes to the Trash, and the message says so.
TEST_F(InboxServiceTest, AnUpgradeOfAFileNoLongerThereTakesItsPlace)
{
    service->setTierLookup(notInCollection);
    const QString old = config.albumsRoot + u"/Okuma Drift - Nightfall/01 Dusk.mp3"_s;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(old).absolutePath()));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, old);
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::Replaces);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"which wasn't there any more"_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_EQ(contents(old), "audio");
    EXPECT_EQ(inbox.count(), 0);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));
}

// Other collection folders chosen since: the file isn't the collection's any more, and stays as it is; the upgrade is
// filed as a new file, as the Inbox said beforehand.
TEST_F(InboxServiceTest, AnUpgradeWhoseFolderLeftTheCollectionIsANewFile)
{
    service->setTierLookup(notInCollection);
    const QString old = dir.filePath(u"Old Music/Okuma Drift - Dusk.mp3"_s);
    ASSERT_TRUE(writeFile(old, "old"));
    const qint64 id = arrive(u"Singles/Okuma Drift - Dusk.mp3"_s);
    markUpgrade(inbox, id, old);
    ASSERT_EQ(service->entries().constFirst().upgrade, UpgradeVerdict::CantReplace);
    const QSignalSpy finished(service.get(), &InboxService::finished);
    service->file({id});
    service->waitForIdle();
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.at(0).at(1).toBool()) << finished.at(0).at(2).toString().toStdString();
    EXPECT_TRUE(finished.at(0).at(2).toString().contains(u"stays as it was"_s))
        << finished.at(0).at(2).toString().toStdString();
    EXPECT_EQ(contents(old), "old");
    EXPECT_EQ(contents(config.tracksRoot + u"/Okuma Drift - Dusk.mp3"_s), "audio");
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(u"Trash"_s)));
}

// An upgrade needs no tracks folder to take an album track's place; filed as a new file, it would.
TEST_F(InboxServiceTest, AnUpgradeNeedsTheTracksFolderOnlyAsANewFile)
{
    service->setTierLookup(collectionAt(QualityTier::Below192));
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
