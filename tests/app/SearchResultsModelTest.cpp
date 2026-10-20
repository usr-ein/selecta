// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/SearchResultsModel.h"

#include <QAbstractItemModelTester>

#include <gtest/gtest.h>

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

    model.setFilter(selecta::core::presetFilter(selecta::core::QualityWanted::Mp3_320OrLossless));
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
    model.sort(SearchResultsModel::AvailabilityColumn, Qt::AscendingOrder);
    EXPECT_EQ(shownOrder(model).constLast(), 0); // the busy peer last
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
}
