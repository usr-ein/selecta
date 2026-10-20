# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for audio/. Test audio is synthetic, generated at test time (support/SyntheticAudio).

find_package(mp3lame CONFIG REQUIRED)

# Synthetic files and decode helpers, shared with tests/convert/.
add_library(selecta_media_test_support STATIC
    support/SyntheticAudio.h support/SyntheticAudio.cpp
    support/DecodeHelpers.h support/DecodeHelpers.cpp)
target_include_directories(selecta_media_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(selecta_media_test_support PUBLIC selecta_audio Qt6::Core mp3lame::mp3lame)
selecta_set_warnings(selecta_media_test_support)

selecta_add_test_suite(selecta_audio_tests
    SOURCES
        AnalysisCacheTest.cpp
        AvioAdapterTest.cpp
        ByteSourceTest.cpp
        DecoderTest.cpp
        ParserTest.cpp
        QualityCheckTest.cpp
        WaveformJobTest.cpp
        WaveformTest.cpp
    LIBRARIES selecta_audio selecta_media_test_support)
# selecta_audio exports vcpkg's include root (FFmpeg's headers live there), which also holds vcpkg's GoogleTest headers.
# Sanitizer builds compile GoogleTest from source instead, so its own headers must come first or the tests compile
# against one GoogleTest and link another.
target_include_directories(selecta_audio_tests SYSTEM BEFORE PRIVATE
    $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>)

# Analysis speed (NFR-2). Not a CTest test: run it from a Release build and record the numbers in src/audio/DESIGN.md.
find_package(benchmark CONFIG REQUIRED)
add_executable(selecta_audio_bench bench/AnalysisBenchmark.cpp)
target_link_libraries(selecta_audio_bench PRIVATE selecta_audio selecta_media_test_support benchmark::benchmark)
selecta_set_warnings(selecta_audio_bench)
