// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/CrashReport.h"

#include <QCoreApplication>
#include <QStandardPaths>

#include <gtest/gtest.h>

int main(int argc, char **argv)
{
    selecta::testing::installCrashReport();
    ::testing::InitGoogleTest(&argc, argv);
    const QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SelectaTests"));
    QStandardPaths::setTestModeEnabled(true);
    return RUN_ALL_TESTS();
}
