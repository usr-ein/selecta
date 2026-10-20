// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/ActivityModel.h"

#include "governor/Decision.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::ActivityModel;

TEST(ActivityModel, OnlyRefusalsAndFailuresAreWarnings)
{
    // What went as it should is never drawn in the warning colour (QA Q14): downloads, arrivals, albums, rooms…
    for (const QString &fine : {u"allowed"_s, u"wanted"_s, u"completed"_s, u"arrived"_s, u"decided"_s, u"undone"_s,
                                u"from-preview"_s, u"joined"_s, u"sent"_s, u"browsed"_s, u"connected"_s}) {
        EXPECT_FALSE(ActivityModel::isWarning(fine)) << fine.toStdString();
    }
    for (const QString &warning : {u"failed"_s, u"cancelled"_s, u"declined"_s, u"denied"_s, u"disconnected"_s}) {
        EXPECT_TRUE(ActivityModel::isWarning(warning)) << warning.toStdString();
    }
    // Every refusal of the governor is a warning.
    using selecta::governor::Refusal;
    for (const Refusal refusal : {Refusal::RateLimited, Refusal::PeerDailyLimit, Refusal::NetworkPaused,
                                  Refusal::MessageRepeated, Refusal::LiveServerRefused}) {
        EXPECT_TRUE(ActivityModel::isWarning(selecta::governor::refusalName(refusal)))
            << selecta::governor::refusalName(refusal).toStdString();
    }
    EXPECT_FALSE(ActivityModel::isWarning(QString())); // nothing to say
}
