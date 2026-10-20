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

TEST(ActivityModel, TheDetailsColumnSaysWhoStartedItAndWhy)
{
    ActivityModel model;
    EXPECT_EQ(model.columnCount(), 5);
    EXPECT_EQ(model.headerData(ActivityModel::Details, Qt::Horizontal).toString(), u"Details"_s);
    model.reset({selecta::core::ActivityEntry{.id = 2,
                                              .atMs = 0,
                                              .operation = u"shares"_s,
                                              .subject = u"x"_s,
                                              .outcome = u"browsed"_s,
                                              .explanation = u"x looked at what you share"_s},
                 selecta::core::ActivityEntry{.id = 1,
                                              .atMs = 0,
                                              .operation = u"search"_s,
                                              .subject = u"neon rain"_s,
                                              .outcome = u"allowed"_s,
                                              .explanation = QString()}});
    const QString sentence = u"x is browsing your shares: they asked for the list of what you share."_s;
    EXPECT_EQ(model.data(model.index(0, ActivityModel::Details)).toString(), sentence);
    EXPECT_EQ(model.data(model.index(0, ActivityModel::Operation), Qt::ToolTipRole).toString(), sentence);
    EXPECT_EQ(model.data(model.index(0, ActivityModel::Details), Qt::AccessibleDescriptionRole).toString(), sentence);
    // Nothing to say: no tooltip at all.
    EXPECT_FALSE(model.data(model.index(1, ActivityModel::Details), Qt::ToolTipRole).isValid());
    EXPECT_TRUE(model.data(model.index(1, ActivityModel::Details)).toString().isEmpty());
}
