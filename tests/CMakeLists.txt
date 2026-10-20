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
        library/QuarantineTest.cpp
        storage/DatabaseTest.cpp
        storage/MigrationsTest.cpp
        storage/SessionLogTest.cpp
    LIBRARIES selecta_base selecta_governor selecta_storage selecta_library)


# Library test suites: tests/<library>/CMakeLists.txt, owned together with src/<library>/ (PLAN §9).
set(_selecta_library_tests slsk meta match)
if(SELECTA_WITH_MEDIA)
    list(APPEND _selecta_library_tests audio convert collection share)
endif()
foreach(_module IN LISTS _selecta_library_tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/CMakeLists.txt")
        add_subdirectory(${_module})
    endif()
endforeach()

# Head implementer's media-dependent modules.
if(SELECTA_WITH_MEDIA)
    selecta_add_test_suite(selecta_core_tests
        SOURCES
            core/ActivityLogTest.cpp
            core/AlbumArrivalTest.cpp
            core/AlbumServiceTest.cpp
            core/ArrivalServiceTest.cpp
            core/BrowseServiceTest.cpp
            core/CollectionServiceTest.cpp
            core/CountryDatabaseTest.cpp
            core/ConverterServiceTest.cpp
            core/DownloadsTest.cpp
            core/EngineTest.cpp
            core/FileRankingTest.cpp
            core/GetTracksServiceTest.cpp
            core/GovernorPacerTest.cpp
            core/GovernorStoreTest.cpp
            core/InboxRetagTest.cpp
            core/InboxServiceTest.cpp
            core/ManualSearchServiceTest.cpp
            core/MatchCorrectionsTest.cpp
            core/MetaServicesTest.cpp
            core/NetworkServiceTest.cpp
            core/PortMapClientTest.cpp
            core/PortMapperTest.cpp
            core/PreviewCacheTest.cpp
            core/ResultAnnotatorTest.cpp
            core/RoomServiceTest.cpp
            core/SearchCacheTest.cpp
            core/SearchFilterTest.cpp
            core/SearchServiceTest.cpp
            core/ShareServiceTest.cpp
            core/SocialServiceTest.cpp
            core/TransferServiceTest.cpp
            core/WishlistServiceTest.cpp
            core/YouTubePreviewsTest.cpp
        LIBRARIES selecta_core selecta_media_test_support selecta_slsk_testing Qt6::Gui ZLIB::ZLIB)
    # The fake yt-dlp and its fixtures live with meta's tests.
    target_compile_definitions(selecta_core_tests PRIVATE
        SELECTA_META_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/meta/fixtures")
    if(SELECTA_STATIC_QT)
        qt_import_plugins(selecta_core_tests INCLUDE Qt6::QJpegPlugin) # covers in the arrival tests
    endif()
    add_subdirectory(playback)
    selecta_add_test_suite(selecta_ui_tests GUI
        SOURCES
            app/AccessibilityTest.cpp
            app/ActivityModelTest.cpp
            app/AppIconTest.cpp
            app/DiagnosticsTest.cpp
            app/ConverterViewTest.cpp
            app/FormatTest.cpp
            app/GetTracksInspectorTest.cpp
            app/GetTracksModelTest.cpp
            app/LibrarySettingsTest.cpp
            app/MainWindowTest.cpp
            app/NavModelTest.cpp
            app/NotifierTest.cpp
            app/PortMappingSettingsTest.cpp
            app/PresetsTest.cpp
            app/PreviewQualityTest.cpp
            app/SearchResultsModelTest.cpp
            app/SpectrogramViewTest.cpp
            app/SpotifyTermsTest.cpp
            app/TickDelegateTest.cpp
            app/TransfersModelTest.cpp
            app/WaveformViewTest.cpp
        LIBRARIES selecta_app selecta_media_test_support)
    if(APPLE)
        # The built bundle's icon (D36): Selecta.icns, and Assets.car when actool compiled the Icon Composer icon.
        add_test(NAME selecta_app_bundle_icon COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/app/check-bundle-icon.sh"
            "$<TARGET_BUNDLE_DIR:Selecta>" "$<BOOL:${SELECTA_ACTOOL}>")
    endif()
    target_compile_definitions(selecta_ui_tests PRIVATE
        SELECTA_META_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/meta/fixtures")
endif()
