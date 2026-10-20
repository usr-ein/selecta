# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the match library (docs/contracts/match.md): the research note's edge-case table as parameterised cases,
# property tests and a concurrency test.
selecta_add_test_suite(selecta_match_tests
    SOURCES
        MatchTestSupport.h
        ConcurrencyTest.cpp
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
