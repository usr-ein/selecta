# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the match library (docs/contracts/match.md): the research note's edge-case table as parameterised cases,
# property tests and a concurrency test.
selecta_add_test_suite(selecta_match_tests
    SOURCES
        MatchTestSupport.h
        AlbumMatcherTest.cpp
        ConcurrencyTest.cpp
        DurationConsensusTest.cpp
        DurationQualityTest.cpp
        MatcherTest.cpp
        NormalizerTest.cpp
        QueryPlannerTest.cpp
        ReleaseScorerTest.cpp
        RemotePathParserTest.cpp
        SimilarityTest.cpp
        TokenIndexTest.cpp
        TracklistTest.cpp
        VersionClassifierTest.cpp
    LIBRARIES selecta_match)

# Google Benchmark on a synthetic 200 k-track library (DESIGN.md, "Tests and benchmark"). Run it from an optimised
# build for real numbers; ctest only runs a small smoke pass so it keeps compiling and working.
find_package(benchmark CONFIG REQUIRED)
add_executable(selecta_match_benchmark MatchBenchmark.cpp)
target_link_libraries(selecta_match_benchmark PRIVATE selecta_match benchmark::benchmark)
selecta_set_warnings(selecta_match_benchmark)
add_test(NAME selecta_match_benchmark_smoke COMMAND selecta_match_benchmark --benchmark_min_time=0.01s)
set_tests_properties(selecta_match_benchmark_smoke PROPERTIES
    ENVIRONMENT "SELECTA_BENCH_TRACKS=2000;SELECTA_BENCH_RESULTS=200"
    LABELS "benchmark")
