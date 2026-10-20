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
