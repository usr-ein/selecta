// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ResultAnnotator.h"

#include "base/Clock.h"
#include "governor/Governor.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::Ownership;
using selecta::core::ResultAnnotator;
using selecta::core::SearchResult;

namespace {

using Queued = QSet<std::pair<QString, QByteArray>>;

class ResultAnnotatorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
    }

    // A collection file of this quality; returns its id.
    qint64 own(const QString &path, selecta::match::Codec codec, int kbps)
    {
        selecta::collection::ScannedFile file;
        file.file = {path, 9'000'000, 1'700'000'000'000};
        file.artist = u"Okuma Drift"_s;
        file.title = u"Neon Rain"_s;
        file.durationMs = 372'000;
        file.quality.codec = codec;
        file.quality.bitrateKbps = kbps;
        selecta::core::CollectionStore store(db);
        EXPECT_TRUE(store.apply({file}, {}, {}));
        const QList<selecta::core::CollectionFile> files = store.all();
        for (const selecta::core::CollectionFile &known : files) {
            if (known.file.file.path == path) {
                return known.id;
            }
        }
        return 0;
    }

    static SearchResult result(Ownership ownership, quint64 ownedId = 0)
    {
        SearchResult r;
        r.hit.peer = u"vinyl_ghost"_s;
        r.hit.path = "@@a\\Okuma Drift - Neon Rain.flac";
        r.ownership = ownership;
        r.ownedId = ownedId;
        return r;
    }

    selecta::storage::Database db;
    selecta::base::ManualClock clock;
    selecta::governor::Governor governor{clock};
};

} // namespace

TEST_F(ResultAnnotatorTest, AnUpgradeOverAnMp3320IsntOne)
{
    const qint64 good = own(u"/music/Singles/Okuma Drift - Neon Rain.mp3"_s, selecta::match::Codec::Mp3, 320);
    const qint64 poor = own(u"/music/Old/Okuma Drift - Neon Rain.mp3"_s, selecta::match::Codec::Mp3, 192);
    const Queued none;
    ResultAnnotator annotator(db, governor, none, 4);
    SearchResult overGood = result(Ownership::Upgrade, static_cast<quint64>(good));
    annotator.annotate(overGood);
    EXPECT_EQ(overGood.ownership, Ownership::InCollection); // it would arrive as the same MP3 320 (D29)
    EXPECT_EQ(overGood.ownershipDetail, u"/music/Singles/Okuma Drift - Neon Rain.mp3"_s);
    SearchResult overPoor = result(Ownership::Upgrade, static_cast<quint64>(poor));
    annotator.annotate(overPoor);
    EXPECT_EQ(overPoor.ownership, Ownership::Upgrade);
    EXPECT_EQ(overPoor.ownershipDetail, u"/music/Old/Okuma Drift - Neon Rain.mp3"_s);
}

TEST_F(ResultAnnotatorTest, FilesInTheTransfersAreQueued)
{
    const Queued queued{{u"vinyl_ghost"_s, QByteArray("@@a\\Okuma Drift - Neon Rain.flac")}};
    ResultAnnotator annotator(db, governor, queued, std::nullopt);
    SearchResult fresh = result(Ownership::None);
    annotator.annotate(fresh);
    EXPECT_EQ(fresh.ownership, Ownership::Queued);
    SearchResult inbox = result(Ownership::InInbox);
    annotator.annotate(inbox);
    EXPECT_EQ(inbox.ownership, Ownership::InInbox); // closer to home wins

    const Queued none;
    ResultAnnotator later(db, governor, none, std::nullopt); // the transfer ended
    later.annotate(fresh);
    EXPECT_EQ(fresh.ownership, Ownership::None);
}

TEST_F(ResultAnnotatorTest, PastDownloadsSayWhenAndWhere)
{
    selecta::storage::SessionLog sessions(db);
    const int session = sessions.start(1'700'000'000'000);
    const int next = sessions.start(1'700'000'100'000);
    selecta::library::HistoryStore history(db);
    selecta::library::HistoryEntry entry;
    entry.session = session;
    entry.completedAt = 1'800'000'000'000;
    entry.peer = u"bassbin"_s;
    entry.remotePath = "@@b\\x.flac";
    const std::optional<qint64> id = history.add(entry);
    ASSERT_TRUE(id.has_value());

    SearchResult now = result(Ownership::DownloadedBefore);
    now.historyIds = {static_cast<quint64>(*id)};
    const Queued none;
    ResultAnnotator thisSession(db, governor, none, session);
    thisSession.annotate(now);
    EXPECT_EQ(now.ownership, Ownership::DownloadedThisSession);
    EXPECT_TRUE(now.ownershipDetail.contains(u"bassbin"_s));

    SearchResult before = result(Ownership::DownloadedBefore);
    before.historyIds = {static_cast<quint64>(*id)};
    ResultAnnotator nextRun(db, governor, none, next);
    nextRun.annotate(before);
    EXPECT_EQ(before.ownership, Ownership::DownloadedBefore);
}

TEST_F(ResultAnnotatorTest, PeersCountersComeFromTheGovernor)
{
    governor.downloadQueued(u"vinyl_ghost"_s);
    governor.downloadStarted(u"vinyl_ghost"_s);
    governor.downloadFinished(u"vinyl_ghost"_s, 9'000'000, true);
    governor.peerDenied(u"vinyl_ghost"_s, u"Banned"_s);
    const Queued none;
    ResultAnnotator annotator(db, governor, none, std::nullopt);
    SearchResult r = result(Ownership::None);
    annotator.annotate(r);
    EXPECT_EQ(r.peerFilesToday, 1);
    EXPECT_GT(r.peerBackoffMs, 0);
}
