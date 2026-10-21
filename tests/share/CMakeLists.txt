# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for share/. Files are synthetic (tests/audio/support makes the real audio), written into temporary folders only.

selecta_add_test_suite(selecta_share_tests
    SOURCES
        ShareTestSupport.h ShareTestSupport.cpp
        BuildTest.cpp
        CliTest.cpp
        ConcurrencyTest.cpp
        LookupTest.cpp
        OpenTest.cpp
        QueryTest.cpp
        SearchTest.cpp
        TextTest.cpp
        UploadQueueTest.cpp
    LIBRARIES selecta_share selecta_media_test_support)
# GoogleTest's headers before vcpkg's include root (see tests/audio/CMakeLists.txt).
target_include_directories(selecta_share_tests SYSTEM BEFORE PRIVATE
    $<TARGET_PROPERTY:GTest::gtest,INTERFACE_INCLUDE_DIRECTORIES>)
# CliTest runs selecta-cli's share index command over temporary folders.
target_compile_definitions(selecta_share_tests PRIVATE SELECTA_CLI_PATH="$<TARGET_FILE:selecta-cli>")
add_dependencies(selecta_share_tests selecta-cli)

# Build, search and lookup speed and the index's memory (contract: 20 k files built in under ~3 s with knownAudio
# complete; under ~60 MB and under ~1 ms per typical query at 100 k files). Run it from an optimised build for real
# numbers and record them in src/share/DESIGN.md; ctest only runs a small smoke pass in a throwaway folder.
find_package(benchmark CONFIG REQUIRED)
add_executable(selecta_share_bench bench/ShareBenchmark.cpp)
target_link_libraries(selecta_share_bench PRIVATE selecta_share benchmark::benchmark Qt6::Core)
selecta_set_warnings(selecta_share_bench)
add_test(NAME selecta_share_bench_smoke COMMAND selecta_share_bench --benchmark_min_time=0.01s)
set_tests_properties(selecta_share_bench_smoke PROPERTIES
    ENVIRONMENT "SELECTA_BENCH_FILES=300;SELECTA_BENCH_SEARCH_FILES=300;SELECTA_BENCH_KEEP=0"
    LABELS "benchmark")
