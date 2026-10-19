# SPDX-License-Identifier: GPL-3.0-or-later
include(GoogleTest)

# Logic tests: no GUI. A QCoreApplication with QStandardPaths in test mode (unit_main.cpp).
add_executable(selecta_unit_tests
    unit_main.cpp
    base/ClockTest.cpp
    base/LoggingTest.cpp
    base/PathsTest.cpp
    governor/ServerGuardTest.cpp
    governor/TokenBucketTest.cpp
    storage/DatabaseTest.cpp
    storage/MigrationsTest.cpp
    storage/SessionLogTest.cpp)
target_link_libraries(selecta_unit_tests PRIVATE selecta_base selecta_governor selecta_storage GTest::gtest)
selecta_set_warnings(selecta_unit_tests)
gtest_discover_tests(selecta_unit_tests DISCOVERY_MODE PRE_TEST)

# UI tests: widgets under Qt's "minimal" platform, which needs no display (ui_main.cpp). The static Qt build has no
# "offscreen" platform.
add_executable(selecta_ui_tests
    ui_main.cpp
    app/MainWindowTest.cpp
    app/NavModelTest.cpp)
target_link_libraries(selecta_ui_tests PRIVATE selecta_app Qt6::Test GTest::gtest)
if(SELECTA_STATIC_QT)
    qt_import_plugins(selecta_ui_tests INCLUDE Qt6::QMinimalIntegrationPlugin)
endif()
selecta_set_warnings(selecta_ui_tests)
gtest_discover_tests(selecta_ui_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
