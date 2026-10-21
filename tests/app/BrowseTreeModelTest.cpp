// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/BrowseTreeModel.h"

#include "core/SearchFilter.h"

#include <QAbstractItemModelTester>
#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QTextDocument>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::BrowseTreeModel;
using selecta::core::HiddenReason;
using selecta::core::SearchFilter;
using selecta::core::SearchHit;
using selecta::core::SearchResult;

namespace {

// A browsed file, as BrowseService makes it: the peer's path and attributes, no slot or queue.
SearchHit hit(const QByteArray &path, quint32 kbps = 320, quint32 seconds = 300)
{
    SearchHit result;
    result.peer = u"vinyl_ghost"_s;
    result.path = path;
    result.extension = QString::fromUtf8(path.mid(path.lastIndexOf('.') + 1)).toLower();
    if (result.extension == u"mp3"_s) {
        result.attributes.bitrate = kbps;
        result.attributes.duration = seconds;
        result.size = (quint64{kbps} * 1000 / 8 * seconds) + 40'000;
    } else {
        result.size = 10'000;
    }
    return result;
}

QList<SearchResult> analysed(const QList<SearchHit> &hits)
{
    return selecta::core::analyzeHits(hits, 0, {}, std::nullopt);
}

QStringList namesUnder(const BrowseTreeModel &model, const QModelIndex &parent = {})
{
    QStringList names;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        names.append(model.index(row, BrowseTreeModel::NameColumn, parent).data().toString());
    }
    return names;
}

QModelIndex child(const BrowseTreeModel &model, const QModelIndex &parent, const QString &name)
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data().toString() == name) {
            return index;
        }
    }
    return {};
}

QList<SearchResult> nested()
{
    return analysed({
        hit(R"(@@music\Okuma Drift\Night Signals\10 - Okuma Drift - Coda.mp3)"),
        hit(R"(@@music\Okuma Drift\Night Signals\2 - Okuma Drift - Low Tide.mp3)"),
        hit(R"(@@music\Okuma Drift\Night Signals\cover.jpg)"),
        hit(R"(@@music\Okuma Drift\Night Signals\CD2\01 - Okuma Drift - Encore.mp3)"),
        hit("@@music\\Okuma Drift\\loose single.mp3", 128),
        hit("@@music\\SPEEDTEST\\10MB.bin"),
        hit("@@music\\SPEEDTEST\\100MB.bin"),
    });
}

} // namespace

TEST(BrowseTreeModel, NestsFoldersAsThePeerHasThem)
{
    BrowseTreeModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(nested());
    EXPECT_EQ(model.fileCount(), 7);
    EXPECT_EQ(model.folderCount(), 5); // @@music, Okuma Drift, Night Signals, CD2, SPEEDTEST
    ASSERT_EQ(namesUnder(model), QStringList{u"@@music"_s});
    const QModelIndex music = model.index(0, 0);
    EXPECT_TRUE(music.data(BrowseTreeModel::IsFolderRole).toBool());
    EXPECT_EQ(namesUnder(model, music), (QStringList{u"Okuma Drift"_s, u"SPEEDTEST"_s}));
    const QModelIndex artist = child(model, music, u"Okuma Drift"_s);
    // Folders first, then files.
    EXPECT_EQ(namesUnder(model, artist), (QStringList{u"Night Signals"_s, u"loose single.mp3"_s}));
    const QModelIndex album = child(model, artist, u"Night Signals"_s);
    // Natural order: 2 before 10; the subfolder first.
    EXPECT_EQ(namesUnder(model, album), (QStringList{u"CD2"_s, u"2 - Okuma Drift - Low Tide.mp3"_s,
                                                     u"10 - Okuma Drift - Coda.mp3"_s, u"cover.jpg"_s}));
    // A folder counts every file under it; its tooltip has its whole path.
    EXPECT_TRUE(album.data(Qt::ToolTipRole).toString().contains(u"@@music\\Okuma Drift\\Night Signals"_s));
    EXPECT_TRUE(album.data(Qt::ToolTipRole).toString().contains(u"4 files"_s));
    EXPECT_FALSE(model.index(album.row(), BrowseTreeModel::SizeColumn, album.parent()).data().toString().isEmpty());
    // Only the files directly in a folder stand for it.
    const QList<int> direct = model.filesIn(album);
    EXPECT_EQ(direct.size(), 3);
    for (const int index : direct) {
        EXPECT_FALSE(model.result(index)->path.contains(u"CD2"_s));
    }
    EXPECT_TRUE(model.filesIn(music).isEmpty());
}

TEST(BrowseTreeModel, EveryFileIsListedAndTheFilteredOnesAreDimmed)
{
    BrowseTreeModel model;
    model.reset(nested());
    // Selecta's default: MP3 320 or lossless, strict.
    model.setFilter(selecta::core::builtInPreset(selecta::core::kMp3OrLosslessPreset)->filter);
    const selecta::core::FilterCounts counts = model.counts();
    EXPECT_EQ(counts.total, 7);
    EXPECT_EQ(counts.shown, 3); // the three MP3 320s
    const QModelIndex speedtest = child(model, model.index(0, 0), u"SPEEDTEST"_s);
    ASSERT_EQ(model.rowCount(speedtest), 2); // listed all the same
    const QModelIndex bin = model.index(0, 0, speedtest);
    EXPECT_EQ(bin.data(BrowseTreeModel::HiddenReasonRole).toInt(), static_cast<int>(HiddenReason::NotAudio));
    EXPECT_EQ(bin.data(Qt::ForegroundRole).value<QColor>(),
              QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text));
    EXPECT_TRUE(bin.data(Qt::ToolTipRole).toString().contains(u"Not audio"_s));
    EXPECT_TRUE(bin.data(Qt::AccessibleTextRole).toString().contains(u"outside your filters"_s));
    const QModelIndex single = model.indexOfPath("@@music\\Okuma Drift\\loose single.mp3");
    ASSERT_TRUE(single.isValid());
    EXPECT_EQ(single.data(BrowseTreeModel::HiddenReasonRole).toInt(), static_cast<int>(HiddenReason::Bitrate));
    const QModelIndex good = model.indexOfPath(R"(@@music\Okuma Drift\Night Signals\2 - Okuma Drift - Low Tide.mp3)");
    ASSERT_TRUE(good.isValid());
    EXPECT_FALSE(good.data(Qt::ForegroundRole).isValid());
    EXPECT_FALSE(model.index(good.row(), BrowseTreeModel::QualityColumn, good.parent()).data().toString().isEmpty());

    // "Any quality" filters nothing; the rows stay where they are.
    model.setFilter(selecta::core::builtInPreset(selecta::core::kAnyQualityPreset)->filter);
    EXPECT_EQ(model.counts().shown, 4); // every audio file: all but the cover and the two .bin
    EXPECT_EQ(single.data(BrowseTreeModel::HiddenReasonRole).toInt(), static_cast<int>(HiddenReason::None));
}

TEST(BrowseTreeModel, FiltersAboutThePeersSlotsDontJudgeABrowse)
{
    BrowseTreeModel model;
    model.reset(nested());
    SearchFilter filter;
    filter.freeSlotOnly = true;
    filter.maxQueue = 1;
    filter.minSpeedKBps = 500;
    model.setFilter(filter);
    EXPECT_EQ(model.counts().shown, 4); // a browse knows nothing of them: only what isn't audio is outside
}

TEST(BrowseTreeModel, FindsAFileByThePathThePeerSent)
{
    BrowseTreeModel model;
    model.reset(nested());
    const QModelIndex found =
        model.indexOfPath(R"(@@music\Okuma Drift\Night Signals\CD2\01 - Okuma Drift - Encore.mp3)");
    ASSERT_TRUE(found.isValid());
    EXPECT_EQ(found.data().toString(), u"01 - Okuma Drift - Encore.mp3"_s);
    EXPECT_EQ(found.parent().data().toString(), u"CD2"_s);
    EXPECT_EQ(found.parent().parent().data().toString(), u"Night Signals"_s);
    EXPECT_EQ(model.resultAt(found)->hit.path,
              QByteArray("@@music\\Okuma Drift\\Night Signals\\CD2\\01 - Okuma Drift - Encore.mp3"));
    EXPECT_FALSE(model.indexOfPath("@@music\\nope.mp3").isValid());
    EXPECT_FALSE(model.indexOfPath({}).isValid());
    // Still found after a sort.
    model.sort(BrowseTreeModel::SizeColumn, Qt::DescendingOrder);
    EXPECT_EQ(
        model.indexOfPath("@@music\\Okuma Drift\\Night Signals\\CD2\\01 - Okuma Drift - Encore.mp3").data().toString(),
        u"01 - Okuma Drift - Encore.mp3"_s);
}

TEST(BrowseTreeModel, AnEmptyListIsAnEmptyTree)
{
    BrowseTreeModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset({});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.fileCount(), 0);
    EXPECT_EQ(model.folderCount(), 0);
    EXPECT_EQ(model.counts().total, 0);
    model.reset(nested());
    model.reset({}); // and back
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(BrowseTreeModel, OddPathsAllLandSomewhere)
{
    BrowseTreeModel model;
    const QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.reset(analysed({
        hit("no folder.mp3"),                // at the top
        hit(R"(\\leading\\\doubled\a.mp3)"), // empty parts skipped
        hit("mixed/slashes\\b.mp3"),         // either separator
        hit("trailing\\"),                   // a "file" with no name
        hit("dup\\same.mp3"),
        hit("dup\\same.mp3"),              // the same path twice: two rows
        hit("<b>bold & \x01weird\\c.mp3"), // markup and control characters stay text
        hit("\xff\xfe latin1\\d.mp3"),     // not UTF-8
    }));
    EXPECT_EQ(model.fileCount(), 8);
    const QStringList top = namesUnder(model);
    EXPECT_TRUE(top.contains(u"no folder.mp3"_s));
    EXPECT_TRUE(top.contains(u"leading"_s));
    EXPECT_TRUE(top.contains(u"mixed"_s));
    EXPECT_EQ(namesUnder(model, child(model, model.index(top.indexOf(u"leading"_s), 0), u"doubled"_s)),
              QStringList{u"a.mp3"_s});
    EXPECT_EQ(model.rowCount(child(model, {}, u"dup"_s)), 2);
    QModelIndex bold;
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.index(row, 0).data().toString().startsWith(u"<b>bold & "_s)) {
            bold = model.index(row, 0);
        }
    }
    ASSERT_TRUE(bold.isValid());
    const QString tip = bold.data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(!Qt::mightBeRichText(tip) || tip.contains(u"&lt;b&gt;"_s)) << tip.toStdString(); // text, not markup
}

TEST(BrowseTreeModel, ADeepTreeDoesntExhaustTheStack)
{
    QByteArray path;
    for (int depth = 0; depth < 5'000; ++depth) {
        path += "d\\";
    }
    BrowseTreeModel model;
    model.reset(analysed({hit(path + "deep.mp3")}));
    EXPECT_EQ(model.folderCount(), 5'000);
    const QModelIndex deep = model.indexOfPath(path + "deep.mp3");
    ASSERT_TRUE(deep.isValid());
    int depth = 0;
    for (QModelIndex up = deep.parent(); up.isValid(); up = up.parent()) {
        ++depth;
    }
    EXPECT_EQ(depth, 5'000);
    model.sort(BrowseTreeModel::NameColumn, Qt::DescendingOrder);
}

TEST(BrowseTreeModel, ALargeListBuildsAndSorts)
{
    QList<SearchHit> hits;
    for (int folder = 0; folder < 200; ++folder) {
        for (int track = 0; track < 100; ++track) {
            hits.append(hit("@@music\\Artist " + QByteArray::number(folder % 20) + "\\Album " +
                            QByteArray::number(folder) + "\\" + QByteArray::number(track) + " - Track.mp3"));
        }
    }
    BrowseTreeModel model;
    model.reset(analysed(hits));
    EXPECT_EQ(model.fileCount(), 20'000);
    EXPECT_EQ(model.folderCount(), 1 + 20 + 200);
    const QModelIndex music = model.index(0, 0);
    EXPECT_EQ(model.rowCount(music), 20);
    EXPECT_EQ(model.index(0, 0, music).data().toString(), u"Artist 0"_s);
    EXPECT_EQ(model.index(2, 0, music).data().toString(), u"Artist 2"_s); // natural: not "Artist 10"
    model.sort(BrowseTreeModel::NameColumn, Qt::DescendingOrder);
    EXPECT_EQ(model.index(0, 0, music).data().toString(), u"Artist 19"_s);
}

TEST(BrowseTreeModel, FoldersStayFirstWhateverTheColumn)
{
    BrowseTreeModel model;
    model.reset(nested());
    const QModelIndex album = child(model, child(model, model.index(0, 0), u"Okuma Drift"_s), u"Night Signals"_s);
    for (int column = 0; column < BrowseTreeModel::ColumnCount; ++column) {
        for (const Qt::SortOrder order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            model.sort(column, order);
            EXPECT_EQ(model.index(0, 0, album).data().toString(), u"CD2"_s) << column;
        }
    }
    model.sort(BrowseTreeModel::LengthColumn, Qt::AscendingOrder);
    model.sort(-1);                           // ignored
    model.sort(BrowseTreeModel::ColumnCount); // ignored
}
