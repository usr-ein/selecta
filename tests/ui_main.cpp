// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/CrashReport.h"

#include <QApplication>
#include <QStandardPaths>

#include <gtest/gtest.h>

int main(int argc, char **argv)
{
    selecta::testing::installCrashReport();
    ::testing::InitGoogleTest(&argc, argv);
    // Widgets work without a display. CTest sets this too; the default lets the binary run on its own.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "minimal");
    }
    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SelectaUiTests"));
    QStandardPaths::setTestModeEnabled(true);
    return RUN_ALL_TESTS();
}
