// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/TransfersModel.h"

#include "app/MeterDelegate.h"
#include "app/models/PeersModel.h"

#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::MeterDelegate;
using selecta::app::PeersModel;
using selecta::app::TransfersModel;
using selecta::core::Download;
using State = Download::State;

namespace {

Download download(qint64 id, const QString &peer, const QByteArray &path, State state, const QString &message = {})
{
    Download result;
    result.id = id;
    result.peer = peer;
    result.remotePath = path;
    result.size = 10'000'000;
    result.state = state;
    result.message = message;
    return result;
}

QString text(const TransfersModel &model, int row, int column, const QModelIndex &parent = {})
{
    return model.index(row, column, parent).data().toString();
}

} // namespace

TEST(TransfersModel, GroupsByFolderThenByPeer)
{
    TransfersModel model;
    model.reset({download(1, u"vinyl_ghost"_s, "Music\\Night Signals\\01 Neon Rain.flac", State::Queued,
                          u"Queued at vinyl_ghost, place 3"_s),
                 download(2, u"vinyl_ghost"_s, "Music\\Night Signals\\02 Low Tide.flac", State::Arrived),
                 download(3, u"aiff_ada"_s, "Singles\\Kassia Moreau - Night Transit.aiff", State::Failed,
                          u"File not shared."_s)},
                {});
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(text(model, 0, TransfersModel::ItemColumn), u"Night Signals · 2 files"_s);
    EXPECT_EQ(text(model, 0, TransfersModel::PeerColumn), u"vinyl_ghost"_s);
    EXPECT_EQ(text(model, 0, TransfersModel::StatusColumn), u"1 of 2 in your Inbox"_s);
    const QModelIndex album = model.index(0, 0);
    ASSERT_EQ(model.rowCount(album), 2);
    EXPECT_EQ(text(model, 0, TransfersModel::ItemColumn, album), u"01 Neon Rain.flac"_s);
    EXPECT_EQ(text(model, 0, TransfersModel::StatusColumn, album), u"Queued at vinyl_ghost, place 3"_s);
    EXPECT_EQ(text(model, 1, TransfersModel::StatusColumn, album), u"In your Inbox"_s);
    EXPECT_EQ(text(model, 0, TransfersModel::StatusColumn, model.index(1, 0)), u"Failed: File not shared."_s);
    EXPECT_EQ(model.downloadsAt(album), (QList<qint64>{1, 2}));

    model.setGrouping(TransfersModel::Grouping::Peer);
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(text(model, 0, TransfersModel::ItemColumn), u"vinyl_ghost · 2 files"_s);
    EXPECT_EQ(text(model, 1, TransfersModel::ItemColumn), u"aiff_ada · 1 file"_s);
}

TEST(TransfersModel, ArrivingFilesShowPercentSpeedAndTimeLeft)
{
    TransfersModel model;
    model.reset({download(1, u"vinyl_ghost"_s, "Music\\a.flac", State::Transferring)}, {{1, {0, 10'000'000}}});
    model.progress(1, 1'000'000, 10'000'000, 1'000);
    model.progress(1, 3'000'000, 10'000'000, 2'000); // 2 MB in a second
    const QSignalSpy changed(&model, &TransfersModel::dataChanged);
    model.flush();
    EXPECT_FALSE(changed.isEmpty());
    const QModelIndex row = model.index(0, TransfersModel::StatusColumn, model.index(0, 0));
    EXPECT_EQ(row.data().toString(), u"30 % · 2.0 MB/s · 0:04 left"_s);
    EXPECT_DOUBLE_EQ(
        model.index(0, TransfersModel::ProgressColumn, model.index(0, 0)).data(MeterDelegate::FractionRole).toDouble(),
        0.3);
    EXPECT_EQ(model.counts().downloading, 1);
}

TEST(TransfersModel, NewDownloadsJoinTheirFolderAndUploadsComeLast)
{
    TransfersModel model;
    model.reset({download(1, u"vinyl_ghost"_s, "Music\\Album\\01.flac", State::Waiting)}, {});
    selecta::core::UploadEntry upload;
    upload.user = u"crate_digger"_s;
    upload.path = u"@@share\\Okuma Drift\\Neon Rain.mp3"_s;
    upload.size = 1000;
    upload.sent = 250;
    upload.active = true;
    model.setUploads({upload});
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(text(model, 1, TransfersModel::ItemColumn), u"Uploads · 1 file"_s);
    EXPECT_EQ(text(model, 0, TransfersModel::ItemColumn, model.index(1, 0)), u"Neon Rain.mp3"_s);

    const QSignalSpy inserted(&model, &TransfersModel::rowsInserted);
    model.update(download(2, u"vinyl_ghost"_s, "Music\\Album\\02.flac", State::Waiting));
    model.update(download(3, u"aiff_ada"_s, "Other\\x.aiff", State::Waiting));
    ASSERT_EQ(inserted.size(), 2);
    EXPECT_EQ(model.rowCount(model.index(0, 0)), 2); // into the album's folder
    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(text(model, 1, TransfersModel::ItemColumn), u"Other · 1 file"_s); // before the uploads
    EXPECT_EQ(text(model, 2, TransfersModel::ItemColumn), u"Uploads · 1 file"_s);
    const TransfersModel::Counts counts = model.counts();
    EXPECT_EQ(counts.waiting, 3);
    EXPECT_EQ(counts.uploading, 1);
}

TEST(PeersModel, SaysWhereEachPeerStandsAgainstTheLimits)
{
    selecta::core::PeerStats busy;
    busy.peer = u"vinyl_ghost"_s;
    busy.todayFiles = 3;
    busy.queued = 2;
    busy.active = 1;
    selecta::core::PeerStats near = busy;
    near.peer = u"aiff_ada"_s;
    near.todayFiles = 25;
    selecta::core::PeerStats away = busy;
    away.peer = u"refusing_rita"_s;
    away.backoffMs = qint64{30} * 24 * 3600 * 1000;
    EXPECT_EQ(PeersModel::statusText(busy), u"1 arriving · 2 queued"_s);
    EXPECT_EQ(PeersModel::statusText(near), u"Near the daily limit"_s);
    EXPECT_EQ(PeersModel::statusText(away), u"Holding off for 30 days"_s);

    PeersModel model;
    model.reset({busy, near}, 0);
    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.index(0, PeersModel::TodayColumn).data().toString(), u"3"_s);
    EXPECT_DOUBLE_EQ(model.index(1, PeersModel::LimitColumn).data(MeterDelegate::FractionRole).toDouble(), 0.5);
    EXPECT_EQ(model.index(1, PeersModel::LimitColumn).data(MeterDelegate::LevelRole).toInt(), 1);
}
