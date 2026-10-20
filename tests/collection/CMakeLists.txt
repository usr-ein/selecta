# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for collection/. Audio is synthetic (tests/audio/support), written into temporary folders only.

add_library(selecta_collection_test_support STATIC
    support/TestTree.h support/TestTree.cpp)
target_include_directories(selecta_collection_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(selecta_collection_test_support PUBLIC selecta_collection selecta_media_test_support
    GTest::gtest)
selecta_set_warnings(selecta_collection_test_support)
# GoogleTest's headers before vcpkg's include root (see tests/audio/CMakeLists.txt).
target_include_directories(selecta_collection_test_support SYSTEM BEFORE PRIVATE
    $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>)

selecta_add_test_suite(selecta_collection_tests
    SOURCES
        AlbumsTest.cpp
        ScannerTest.cpp
        WatcherTest.cpp
    LIBRARIES selecta_collection selecta_collection_test_support)
target_include_directories(selecta_collection_tests SYSTEM BEFORE PRIVATE
    $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>)
