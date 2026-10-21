// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/GetTracksModel.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::GetTracksModel;
using selecta::core::GetLine;
using Kind = GetTracksModel::StatusKind;

namespace {

GetLine line(GetLine::ReleaseState release, GetLine::FindState find)
{
    GetLine result;
    result.id = 1;
    result.artist = u"Kel Hale"_s;
    result.title = u"Hollow Tide"_s;
    result.releaseState = release;
    result.findState = find;
    return result;
}

} // namespace

TEST(GetTracksModel, ALineWaitingToSearchIsNeverDrawnAsAFailure)
{
    // The release lookup failed, but the line still goes to Soulseek by name: waiting is not a failure (QA Q4).
    const GetLine waiting = line(GetLine::ReleaseState::Failed, GetLine::FindState::Queued);
    EXPECT_EQ(GetTracksModel::statusText(waiting), u"Waiting to search"_s);
    EXPECT_EQ(GetTracksModel::statusKind(waiting), Kind::Waiting);

    // Before Find Them, the failed lookup is what the line has to say.
    const GetLine idle = line(GetLine::ReleaseState::Failed, GetLine::FindState::Idle);
    EXPECT_EQ(GetTracksModel::statusText(idle), u"Lookup failed"_s);
    EXPECT_EQ(GetTracksModel::statusKind(idle), Kind::Bad);

    EXPECT_EQ(GetTracksModel::statusKind(line(GetLine::ReleaseState::Failed, GetLine::FindState::Searching)),
              Kind::Working);
    EXPECT_EQ(GetTracksModel::statusKind(line(GetLine::ReleaseState::Failed, GetLine::FindState::Ready)), Kind::Good);
    EXPECT_EQ(GetTracksModel::statusKind(line(GetLine::ReleaseState::Resolved, GetLine::FindState::NotFound)),
              Kind::Bad);
}

TEST(GetTracksModel, AStuckQueueIsShownPlainly)
{
    GetLine queued = line(GetLine::ReleaseState::Resolved, GetLine::FindState::Ready);
    queued.downloadState = GetLine::DownloadState::Queued;
    EXPECT_EQ(GetTracksModel::statusText(queued), u"Queued at the peer"_s);
    EXPECT_EQ(GetTracksModel::statusKind(queued), Kind::Working);

    GetLine stuck = queued;
    stuck.stuck = true;
    stuck.message = u"No progress in BIRDMANBRR's queue since 21:03"_s;
    EXPECT_EQ(GetTracksModel::statusText(stuck), u"Stuck at the peer"_s);
    EXPECT_EQ(GetTracksModel::statusKind(stuck), Kind::Check);
    // Still on its way: counted with the downloads.
    GetTracksModel model;
    model.reset({stuck});
    EXPECT_EQ(model.counts().downloading, 1);
    EXPECT_EQ(model.data(model.index(0, GetTracksModel::StatusColumn), Qt::ToolTipRole).toString(), stuck.message);
}

// P-010: a line the Collection view asked for says it looks for a better copy, and of which file.
TEST(GetTracksModel, AnUpgradeLineSaysSo)
{
    GetLine upgrade = line(GetLine::ReleaseState::None, GetLine::FindState::Searching);
    upgrade.raw = u"Kel Hale - Hollow Tide"_s;
    upgrade.upgradeOf = u"/Music/Tracks/Kel Hale - Hollow Tide.mp3"_s;
    EXPECT_TRUE(GetTracksModel::subtitle(upgrade).startsWith(u"Upgrade · No release found"_s))
        << GetTracksModel::subtitle(upgrade).toStdString();
    GetTracksModel model;
    model.reset({upgrade});
    const QString tip = model.data(model.index(0, GetTracksModel::TrackColumn), Qt::ToolTipRole).toString();
    EXPECT_TRUE(tip.contains(u"Kel Hale - Hollow Tide.mp3"_s)) << tip.toStdString();

    GetLine ordinary = upgrade;
    ordinary.upgradeOf.clear();
    EXPECT_FALSE(GetTracksModel::subtitle(ordinary).contains(u"Upgrade"_s));
}
