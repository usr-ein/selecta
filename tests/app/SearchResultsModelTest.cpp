// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/SearchResultsModel.h"

#include "app/Format.h"

#include <QAbstractItemModelTester>
#include <QColor>
#include <QSet>

#include <gtest/gtest.h>

#include <limits>

using namespace Qt::StringLiterals;
using selecta::app::SearchResultsModel;
using selecta::core::SearchHit;
using selecta::core::SearchResult;

namespace {

SearchHit hit(const QString &peer, const QString &path, quint32 kbps, quint32 seconds, bool freeSlot = true)
{
    SearchHit result;
    result.peer = peer;
    result.path = path.toUtf8();
    result.extension = path.mid(path.lastIndexOf(u'.') + 1).toLower();
    result.attributes.duration = seconds;
    if (result.extension == u"flac"_s) {
        result.attributes.bitDepth = 16;
        result.attributes.sampleRate = 44'100;
        result.size = quint64{seconds} * 110'000;
    } else {
        result.attributes.bitrate = kbps;
        result.size = (quint64{kbps} * 1000 / 8 * seconds) + 40'000;
    }
    result.freeSlot = freeSlot;
    result.queueLength = freeSlot ? 0 : 7;
    return result;
}

selecta::match::ReleaseReference neonRain()
{
    selecta::match::ReleaseReference reference;
    reference.artists = {u"Okuma Drift"_s};
    reference.title = u"Neon Rain"_s;
    reference.durationMs = 372'000;
    return reference;
}

QList<SearchResult> results(const std::optional<selecta::match::ReleaseReference> &release = {})
{
    return selecta::core::analyzeHits(
        {
            hit(u"bassbin"_s, u"@@b\\Okuma Drift\\Nightfall\\01 Okuma Drift - Neon Rain.mp3"_s, 320, 372, false),
            hit(u"vinyl_ghost"_s, u"@@a\\Okuma Drift - Neon Rain (Extended Mix).flac"_s, 0, 455),
            hit(u"vinyl_ghost"_s, u"@@a\\Okuma Drift - Neon Rain.flac"_s, 0, 373),
            hit(u"bassbin"_s, u"@@b\\Okuma Drift\\Nightfall\\02 Okuma Drift - Tidewater.mp3"_s, 192, 301),
            hit(u"bassbin"_s, u"@@b\\Okuma Drift\\Nightfall\\cover.jpg"_s, 0, 0),
        },
        0, {}, release);
}

// A free-slot or queued hit of `size` bytes at `speed` bytes per second.
SearchHit peerHit(const QString &peer, quint32 speed, bool freeSlot, quint32 queueLength, quint64 size = 10'000'000)
{
    SearchHit result = hit(peer, u"@@x\\Okuma Drift - Neon Rain.mp3"_s, 320, 250, freeSlot);
    result.averageSpeed = speed;
    result.queueLength = queueLength;
    result.size = size;
    return result;
}

QList<int> shownOrder(const SearchResultsModel &model)
{
    QList<int> order;
    for (int row = 0; row < model.rowCount(); ++row) {
        order.append(model.index(row, 0).data(SearchResultsModel::ResultIndexRole).toInt());
    }
    return order;
}

} // namespace

TEST(SearchResultsModelTest, FiltersAndCountsWhyResultsAreHidden)
{
    SearchResultsModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(results());
    EXPECT_EQ(model.rowCount(), 4); // every audio file; the cover is not a track
    EXPECT_EQ(model.counts().total, 5);
    EXPECT_EQ(model.counts().hidden.at(static_cast<std::size_t>(selecta::core::HiddenReason::NotAudio)), 1);

    model.setFilter(selecta::core::builtInPreset(selecta::core::kMp3OrLosslessPreset).value().filter);
    EXPECT_EQ(model.rowCount(), 3); // the 192 kbps MP3 goes
    EXPECT_EQ(model.counts().shown, 3);
    EXPECT_EQ(model.counts().hidden.at(static_cast<std::size_t>(selecta::core::HiddenReason::Bitrate)), 1);
}

TEST(SearchResultsModelTest, BestMatchPutsTheReleaseFirst)
{
    SearchResultsModel model;
    model.reset(results(neonRain()));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    model.setReleaseChosen(true, 372'000);
    const QList<int> order = shownOrder(model);
    ASSERT_EQ(order.size(), 4);
    // The FLAC of this release, then its MP3 (lower quality), then another version and another track.
    EXPECT_EQ(order.at(0), 2);
    EXPECT_EQ(order.at(1), 0);
    EXPECT_EQ(model.index(0, SearchResultsModel::LengthColumn).data().toString(), u"6:13 ✓"_s);
    EXPECT_TRUE(model.index(2, SearchResultsModel::LengthColumn).data().toString().contains(u"s"_s));

    // "Only this release" leaves the two files of it.
    model.setFilter(selecta::core::SearchFilter{});
    EXPECT_EQ(shownOrder(model), (QList<int>{2, 0}));
}

TEST(SearchResultsModelTest, ColumnsSortBothWays)
{
    SearchResultsModel model;
    model.reset(results());
    model.sort(SearchResultsModel::LengthColumn, Qt::AscendingOrder);
    EXPECT_EQ(shownOrder(model).constFirst(), 3); // 5:01
    model.sort(SearchResultsModel::LengthColumn, Qt::DescendingOrder);
    EXPECT_EQ(shownOrder(model).constFirst(), 1); // 7:35
    model.sort(SearchResultsModel::QueueColumn, Qt::AscendingOrder);
    EXPECT_EQ(shownOrder(model).constLast(), 0); // the busy peer last
    model.sort(SearchResultsModel::QueueColumn, Qt::DescendingOrder);
    EXPECT_EQ(shownOrder(model).constFirst(), 0);
    model.sort(SearchResultsModel::kBestMatch);
    EXPECT_EQ(model.sortColumn(), SearchResultsModel::kBestMatch);
}

TEST(SearchResultsModelTest, GroupsByFolder)
{
    SearchResultsModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(results());
    model.setGrouped(true);
    ASSERT_EQ(model.rowCount(), 2); // bassbin's album folder and vinyl_ghost's loose files
    int files = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex folder = model.index(row, 0);
        EXPECT_TRUE(folder.data(SearchResultsModel::IsFolderRole).toBool());
        EXPECT_EQ(model.resultAt(folder), nullptr);
        files += model.rowCount(folder);
        EXPECT_EQ(model.resultsUnder(folder).size(), model.rowCount(folder));
    }
    EXPECT_EQ(files, 4);

    const QModelIndex found = model.indexOfResult(3);
    ASSERT_TRUE(found.isValid());
    EXPECT_EQ(model.resultAt(found)->index, 3);
    EXPECT_TRUE(found.parent().isValid());
    // Grouped rows lead with the track number.
    EXPECT_TRUE(found.siblingAtColumn(SearchResultsModel::TitleColumn).data().toString().startsWith(u"02"_s));
}

TEST(SearchResultsModelTest, StreamedResultsShowAtTheNextRefresh)
{
    SearchResultsModel model;
    const QList<SearchResult> all = results();
    model.reset(all.mid(0, 2));
    EXPECT_EQ(model.rowCount(), 2);
    model.append(all.mid(2));
    EXPECT_TRUE(model.isStale());
    EXPECT_EQ(model.rowCount(), 2); // not yet
    model.refresh();
    EXPECT_FALSE(model.isStale());
    EXPECT_EQ(model.rowCount(), 4);

    SearchResult owned = all.at(0);
    owned.ownership = selecta::core::Ownership::InCollection;
    model.update({owned});
    const QModelIndex row = model.indexOfResult(0, SearchResultsModel::StatusColumn);
    EXPECT_EQ(row.data().toString(), SearchResultsModel::statusText(selecta::core::Ownership::InCollection));
    EXPECT_TRUE(row.data(Qt::ToolTipRole)
                    .toString()
                    .startsWith(selecta::core::ownershipLabel(selecta::core::Ownership::InCollection)));
}

TEST(SearchResultsModelTest, QualitySaysWhatsEstimatedOrSuspicious)
{
    SearchHit silent = hit(u"p"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372);
    silent.attributes.bitrate.reset();
    SearchHit liar = hit(u"p"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372);
    liar.size = quint64{128} * 1000 / 8 * 372;
    const QList<SearchResult> analysed = selecta::core::analyzeHits({silent, liar}, 0, {}, {});
    EXPECT_TRUE(SearchResultsModel::qualityText(analysed.at(0)).contains(u"≈"_s));
    EXPECT_TRUE(SearchResultsModel::qualityText(analysed.at(1)).startsWith(u"⚠"_s));

    // VoiceOver hears the ≈ and the ⚠ in words.
    SearchResultsModel model;
    model.reset(analysed);
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    QStringList spoken;
    for (int row = 0; row < model.rowCount(); ++row) {
        spoken.append(model.index(row, SearchResultsModel::QualityColumn).data(Qt::AccessibleTextRole).toString());
    }
    EXPECT_TRUE(spoken.filter(u"kbps"_s).size() == 1 && spoken.filter(u"about"_s).size() == 1)
        << spoken.join(u'|').toStdString();
    EXPECT_EQ(spoken.filter(u", doubtful"_s).size(), 1) << spoken.join(u'|').toStdString();
    EXPECT_TRUE(spoken.filter(u"≈"_s).isEmpty() && spoken.filter(u"⚠"_s).isEmpty());
}

TEST(SearchResultsModelTest, VoiceOverReadsTheSymbolsInWords)
{
    SearchResultsModel model;
    model.reset(results(neonRain()));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    model.setReleaseChosen(true, 372'000);
    const auto spoken = [&model](int row, int column) {
        return model.index(row, column).data(Qt::AccessibleTextRole).toString();
    };
    EXPECT_EQ(spoken(0, SearchResultsModel::LengthColumn), u"6:13, same length as the release"_s);
    EXPECT_EQ(model.indexOfResult(1, SearchResultsModel::LengthColumn).data(Qt::AccessibleTextRole).toString(),
              u"7:35, +83 s against the release"_s); // the extended mix
    EXPECT_EQ(spoken(0, SearchResultsModel::PeerColumn), u"vinyl_ghost"_s);
    // Columns without symbols read as they show.
    EXPECT_EQ(spoken(0, SearchResultsModel::TitleColumn),
              model.index(0, SearchResultsModel::TitleColumn).data().toString());
    // A cell's tooltip is its description.
    EXPECT_EQ(model.index(0, SearchResultsModel::TitleColumn).data(Qt::AccessibleDescriptionRole),
              model.index(0, SearchResultsModel::TitleColumn).data(Qt::ToolTipRole));
}

TEST(SearchResultsModelTest, ExpectedWaitCombinesSpeedSlotAndQueue)
{
    using selecta::core::expectedWaitSeconds;
    // A free slot: the file alone, at the peer's speed.
    EXPECT_DOUBLE_EQ(expectedWaitSeconds(peerHit(u"a"_s, 1'000'000, true, 0)), 10.0);
    // A free slot wins over a queue reported alongside it: the transfer starts now.
    EXPECT_DOUBLE_EQ(expectedWaitSeconds(peerHit(u"a"_s, 1'000'000, true, 40)), 10.0);
    // Each queued file counts as a typical track ahead of ours.
    EXPECT_DOUBLE_EQ(expectedWaitSeconds(peerHit(u"a"_s, 1'000'000, false, 3)),
                     (3.0 * selecta::core::kTypicalQueuedFileBytes + 10'000'000) / 1'000'000);
    // No free slot and no queue: still one file ahead.
    EXPECT_DOUBLE_EQ(expectedWaitSeconds(peerHit(u"a"_s, 1'000'000, false, 0)),
                     expectedWaitSeconds(peerHit(u"a"_s, 1'000'000, false, 1)));
    // An unknown speed is taken as slow.
    EXPECT_DOUBLE_EQ(expectedWaitSeconds(peerHit(u"a"_s, 0, true, 0)),
                     10'000'000.0 / selecta::core::kUnknownSpeedBytesPerSecond);
    // The largest values stay finite.
    EXPECT_GT(expectedWaitSeconds(peerHit(u"a"_s, 1, false, std::numeric_limits<quint32>::max(),
                                          std::numeric_limits<quint64>::max())),
              0.0);
}

TEST(SearchResultsModelTest, DefaultOrderIsTheShortestWaitFirst)
{
    SearchResultsModel model;
    model.reset(selecta::core::analyzeHits(
        {
            peerHit(u"slow_free"_s, 50'000, true, 0),     // 200 s
            peerHit(u"fast_queued"_s, 5'000'000, false, 20), // 42 s
            peerHit(u"fast_free"_s, 2'000'000, true, 0),  // 5 s
            peerHit(u"silent"_s, 0, true, 0),             // unknown speed: 500 s
            peerHit(u"long_queue"_s, 1'000'000, false, 300), // 3010 s
        },
        0, {}, {}));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    EXPECT_EQ(model.sortColumn(), SearchResultsModel::kBestMatch);
    EXPECT_EQ(shownOrder(model), (QList<int>{2, 1, 0, 3, 4}));

    // The Speed column: fastest first, then back.
    model.sort(SearchResultsModel::SpeedColumn, Qt::AscendingOrder);
    EXPECT_EQ(shownOrder(model), (QList<int>{1, 2, 4, 0, 3}));
    model.sort(SearchResultsModel::SpeedColumn, Qt::DescendingOrder);
    EXPECT_EQ(shownOrder(model).constFirst(), 3);
    // The Q column: free slots (fastest first), then the shorter queue.
    model.sort(SearchResultsModel::QueueColumn, Qt::AscendingOrder);
    EXPECT_EQ(shownOrder(model), (QList<int>{2, 0, 3, 1, 4}));
}

TEST(SearchResultsModelTest, DefaultOrderKeepsTheReleaseFirst)
{
    QList<SearchHit> hits = {
        hit(u"slow"_s, u"@@a\\Okuma Drift - Neon Rain.mp3"_s, 320, 372),
        hit(u"fast"_s, u"@@a\\Okuma Drift - Neon Rain (Extended Mix).mp3"_s, 320, 455),
        hit(u"faster"_s, u"@@a\\Okuma Drift - Neon Rain.flac"_s, 0, 373),
    };
    hits[0].averageSpeed = 10'000;
    hits[1].averageSpeed = 9'000'000;
    hits[2].averageSpeed = 900'000;
    SearchResultsModel model;
    model.reset(selecta::core::analyzeHits(hits, 0, {}, neonRain()));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    model.setReleaseChosen(true, 372'000);
    // The two files of the release first, the faster of them first; the other version, however fast, after.
    EXPECT_EQ(shownOrder(model), (QList<int>{2, 0, 1}));
}

TEST(SearchResultsModelTest, QueueColumnColoursByPresetCutoffs)
{
    using Level = SearchResultsModel::QueueLevel;
    const quint32 shortQueue = SearchResultsModel::kShortQueue;
    const quint32 longQueue = SearchResultsModel::kLongQueue;
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, true, 0)), Level::None);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, true, 500)), Level::None);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, false, 0)), Level::Short);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, false, shortQueue)), Level::Short);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, false, shortQueue + 1)), Level::Long);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, false, longQueue)), Level::Long);
    EXPECT_EQ(SearchResultsModel::queueLevel(peerHit(u"a"_s, 1, false, longQueue + 1)), Level::VeryLong);
    // Four distinct colours.
    QSet<QRgb> colours;
    for (const Level level : {Level::None, Level::Short, Level::Long, Level::VeryLong}) {
        colours.insert(SearchResultsModel::queueColour(level).rgb());
    }
    EXPECT_EQ(colours.size(), 4);

    SearchResultsModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(selecta::core::analyzeHits(
        {peerHit(u"free"_s, 1'000'000, true, 0), peerHit(u"busy"_s, 1'000'000, false, 272)}, 0, {}, {}));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    const QModelIndex free = model.indexOfResult(0, SearchResultsModel::QueueColumn);
    const QModelIndex busy = model.indexOfResult(1, SearchResultsModel::QueueColumn);
    EXPECT_EQ(free.data().toString(), u"NQ"_s);
    EXPECT_EQ(busy.data().toString(), u"Q"_s);
    EXPECT_EQ(free.data(SearchResultsModel::QueueLevelRole).toInt(), static_cast<int>(Level::None));
    EXPECT_EQ(busy.data(SearchResultsModel::QueueLevelRole).toInt(), static_cast<int>(Level::VeryLong));
    EXPECT_EQ(free.data(Qt::ForegroundRole).value<QColor>(), SearchResultsModel::queueColour(Level::None));
    EXPECT_EQ(busy.data(Qt::ForegroundRole).value<QColor>(), SearchResultsModel::queueColour(Level::VeryLong));
    // Hovering gives the exact queue length; VoiceOver reads it too.
    EXPECT_TRUE(busy.data(Qt::ToolTipRole).toString().contains(u"272"_s));
    EXPECT_TRUE(busy.data(Qt::AccessibleTextRole).toString().contains(u"272"_s));
    EXPECT_FALSE(free.data(Qt::ToolTipRole).toString().isEmpty());
    // Other columns aren't coloured.
    EXPECT_FALSE(model.indexOfResult(1, SearchResultsModel::TitleColumn).data(Qt::ForegroundRole).isValid());
}

TEST(SearchResultsModelTest, SpeedColumnShowsTheUploadSpeed)
{
    SearchResultsModel model;
    model.reset(selecta::core::analyzeHits({peerHit(u"fast"_s, 1'200'000, true, 0), peerHit(u"silent"_s, 0, true, 0)},
                                           0, {}, {}));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    const QModelIndex fast = model.indexOfResult(0, SearchResultsModel::SpeedColumn);
    const QModelIndex silent = model.indexOfResult(1, SearchResultsModel::SpeedColumn);
    EXPECT_EQ(fast.data().toString(), selecta::app::format::speed(1'200'000));
    EXPECT_EQ(silent.data().toString(), u"—"_s);
    EXPECT_FALSE(silent.data(Qt::AccessibleTextRole).toString().contains(u"—"_s));
    EXPECT_EQ(model.headerData(SearchResultsModel::QueueColumn, Qt::Horizontal).toString(), u"Q"_s);
    EXPECT_EQ(model.headerData(SearchResultsModel::SpeedColumn, Qt::Horizontal).toString(), u"Speed"_s);
}

TEST(SearchResultsModelTest, FolderRowsShowTheirPeersQueueAndSpeed)
{
    SearchResultsModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(selecta::core::analyzeHits({peerHit(u"busy"_s, 1'200'000, false, 30)}, 0, {}, {}));
    model.setFilter(selecta::core::SearchFilter{.onlyThisRelease = false});
    model.setGrouped(true);
    ASSERT_EQ(model.rowCount(), 1);
    const QModelIndex queue = model.index(0, SearchResultsModel::QueueColumn);
    EXPECT_EQ(queue.data().toString(), u"Q"_s);
    EXPECT_EQ(queue.data(Qt::ForegroundRole).value<QColor>(),
              SearchResultsModel::queueColour(SearchResultsModel::QueueLevel::Long));
    EXPECT_TRUE(queue.data(Qt::ToolTipRole).toString().contains(u"30"_s));
    EXPECT_EQ(model.index(0, SearchResultsModel::SpeedColumn).data().toString(), selecta::app::format::speed(1'200'000));
}
