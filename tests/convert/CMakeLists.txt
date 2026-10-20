# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for convert/. Audio and images are synthetic, generated at test time.

find_package(taglib CONFIG REQUIRED) # the tests also read tags back through TagLib directly

add_library(selecta_convert_test_support STATIC
    support/ConvertTestUtil.h support/ConvertTestUtil.cpp
    support/FailingDevice.h support/FailingDevice.cpp)
target_include_directories(selecta_convert_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(selecta_convert_test_support PUBLIC selecta_convert Qt6::Gui)
selecta_set_warnings(selecta_convert_test_support)

selecta_add_test_suite(selecta_convert_tests
    SOURCES
        EncodeSettingsTest.cpp
        MetadataTest.cpp
        OutputVerifierTest.cpp
        QuantizerTest.cpp
        TagWriterTest.cpp
        TranscoderTest.cpp
    LIBRARIES selecta_convert selecta_convert_test_support selecta_media_test_support TagLib::tag)
# GoogleTest's headers before vcpkg's include root (see tests/audio/CMakeLists.txt).
target_include_directories(selecta_convert_tests SYSTEM BEFORE PRIVATE
    $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>)
if(SELECTA_STATIC_QT)
    # JPEG covers: the static Qt needs its image plugin linked explicitly (head implementer's note).
    qt_import_plugins(selecta_convert_tests INCLUDE Qt6::QJpegPlugin)
endif()

# Conversion speed (sizes the conversion workers, CONV-8). Not a CTest test: run it from a Release build and record the
# numbers in src/convert/DESIGN.md.
find_package(benchmark CONFIG REQUIRED)
add_executable(selecta_convert_bench bench/ConvertBenchmark.cpp)
target_link_libraries(selecta_convert_bench PRIVATE selecta_convert selecta_media_test_support benchmark::benchmark)
selecta_set_warnings(selecta_convert_bench)
