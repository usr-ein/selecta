# SPDX-License-Identifier: GPL-3.0-or-later
include(GoogleTest)

# selecta_add_test_suite(<name> SOURCES <files…> LIBRARIES <targets…> [GUI])
# One GoogleTest executable. Without GUI it runs under a QCoreApplication (unit_main.cpp); with GUI under a
# QApplication on Qt's "minimal" platform (ui_main.cpp). tests/ is on the include path, for tests/support/.
# GoogleTest comes first so its headers win over the copy in vcpkg's include directory, which sanitizer builds
# don't link (they compile GoogleTest from source: cmake/SelectaDependencies.cmake).
function(selecta_add_test_suite name)
    cmake_parse_arguments(ARG "GUI" "" "SOURCES;LIBRARIES" ${ARGN})
    if(ARG_GUI)
        add_executable(${name} "${PROJECT_SOURCE_DIR}/tests/ui_main.cpp" ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE GTest::gtest ${ARG_LIBRARIES} Qt6::Widgets Qt6::Test)
        if(SELECTA_STATIC_QT)
            qt_import_plugins(${name} INCLUDE Qt6::QMinimalIntegrationPlugin)
        endif()
        set(_environment "QT_QPA_PLATFORM=minimal")
    else()
        add_executable(${name} "${PROJECT_SOURCE_DIR}/tests/unit_main.cpp" ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE GTest::gtest ${ARG_LIBRARIES} Qt6::Core Qt6::Test)
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
        core/ActivityLogTest.cpp
        core/EngineTest.cpp
        core/FileRankingTest.cpp
        core/GetTracksServiceTest.cpp
        core/GovernorPacerTest.cpp
        core/GovernorStoreTest.cpp
        core/InboxServiceTest.cpp
        core/MetaServicesTest.cpp
        core/NetworkServiceTest.cpp
        core/SearchServiceTest.cpp
        governor/GovernorTest.cpp
        governor/PeerLedgerTest.cpp
        governor/QueryHygieneTest.cpp
        governor/ReconnectPolicyTest.cpp
        governor/ServerGuardTest.cpp
        governor/TokenBucketTest.cpp
        library/FileOpsTest.cpp
        library/HistoryStoreTest.cpp
        library/InboxStoreTest.cpp
        library/NamingTest.cpp
        storage/DatabaseTest.cpp
        storage/MigrationsTest.cpp
        storage/SessionLogTest.cpp
    LIBRARIES selecta_base selecta_governor selecta_storage selecta_library selecta_core)


# Library test suites: tests/<library>/CMakeLists.txt, owned together with src/<library>/ (PLAN §9).
set(_selecta_library_tests slsk meta match)
if(SELECTA_WITH_MEDIA)
    list(APPEND _selecta_library_tests audio convert collection)
endif()
foreach(_module IN LISTS _selecta_library_tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/CMakeLists.txt")
        add_subdirectory(${_module})
    endif()
endforeach()

# Head implementer's media-dependent modules.
if(SELECTA_WITH_MEDIA)
    add_subdirectory(playback)
    selecta_add_test_suite(selecta_ui_tests GUI
        SOURCES
            app/FormatTest.cpp
            app/MainWindowTest.cpp
            app/NavModelTest.cpp
            app/WaveformViewTest.cpp
        LIBRARIES selecta_app)
endif()
