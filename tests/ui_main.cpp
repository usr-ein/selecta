// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/CrashReport.h"

#include <QApplication>
#include <QStandardPaths>
#include <QTest>

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
    // A failing QCOMPARE, QVERIFY or QTRY_* throws, so GoogleTest fails the test. Without it the macro only returns
    // from the test body, and the test passes: no QTest logger runs outside QTest::qExec().
    QTest::setThrowOnFail(true);
    QApplication::setApplicationName(QStringLiteral("SelectaUiTests"));
    QStandardPaths::setTestModeEnabled(true);
    return RUN_ALL_TESTS();
}
