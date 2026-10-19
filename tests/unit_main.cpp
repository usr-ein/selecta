// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QStandardPaths>

#include <gtest/gtest.h>

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SelectaTests"));
    QStandardPaths::setTestModeEnabled(true);
    return RUN_ALL_TESTS();
}
