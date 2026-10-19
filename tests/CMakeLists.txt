# SPDX-License-Identifier: GPL-3.0-or-later
include(GoogleTest)

# selecta_add_test_suite(<name> SOURCES <files…> LIBRARIES <targets…> [GUI])
# One GoogleTest executable. Without GUI it runs under a QCoreApplication (unit_main.cpp); with GUI under a
# QApplication on Qt's "minimal" platform (ui_main.cpp). tests/ is on the include path, for tests/support/.
function(selecta_add_test_suite name)
    cmake_parse_arguments(ARG "GUI" "" "SOURCES;LIBRARIES" ${ARGN})
    if(ARG_GUI)
        add_executable(${name} "${PROJECT_SOURCE_DIR}/tests/ui_main.cpp" ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE ${ARG_LIBRARIES} Qt6::Widgets Qt6::Test GTest::gtest)
        if(SELECTA_STATIC_QT)
            qt_import_plugins(${name} INCLUDE Qt6::QMinimalIntegrationPlugin)
        endif()
        set(_environment "QT_QPA_PLATFORM=minimal")
    else()
        add_executable(${name} "${PROJECT_SOURCE_DIR}/tests/unit_main.cpp" ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE ${ARG_LIBRARIES} Qt6::Core Qt6::Test GTest::gtest)
        set(_environment "")
    endif()
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    selecta_set_warnings(${name})
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "${_environment}")
endfunction()

# Head implementer's modules.
selecta_add_test_suite(selecta_unit_tests
    SOURCES
        base/ClockTest.cpp
        base/LoggingTest.cpp
        base/PathsTest.cpp
        governor/GovernorTest.cpp
        governor/PeerLedgerTest.cpp
        governor/QueryHygieneTest.cpp
        governor/ReconnectPolicyTest.cpp
        governor/ServerGuardTest.cpp
        governor/TokenBucketTest.cpp
        library/FileOpsTest.cpp
        library/InboxStoreTest.cpp
        storage/DatabaseTest.cpp
        storage/MigrationsTest.cpp
        storage/SessionLogTest.cpp
    LIBRARIES selecta_base selecta_governor selecta_storage selecta_library)

selecta_add_test_suite(selecta_ui_tests GUI
    SOURCES
        app/MainWindowTest.cpp
        app/NavModelTest.cpp
    LIBRARIES selecta_app)

# Library test suites: tests/<library>/CMakeLists.txt, owned together with src/<library>/ (PLAN §9).
set(_selecta_library_tests slsk meta match)
if(SELECTA_WITH_MEDIA)
    list(APPEND _selecta_library_tests audio convert)
endif()
foreach(_module IN LISTS _selecta_library_tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/CMakeLists.txt")
        add_subdirectory(${_module})
    endif()
endforeach()
