# SPDX-License-Identifier: GPL-3.0-or-later
# Name matching and ranking (docs/contracts/match.md, ADR-008). Pure logic on Qt Core and rapidfuzz-cpp (MIT,
# header-only, from vcpkg): no I/O, no database, no network, no UI.
find_package(rapidfuzz CONFIG REQUIRED)

qt_add_library(selecta_match STATIC
    Types.h Types.cpp
    Duration.h Duration.cpp
    Quality.h Quality.cpp
    Lexicon.h Lexicon.cpp
    Normalizer.h Normalizer.cpp
    VersionClassifier.h VersionClassifier.cpp
    Similarity.h Similarity.cpp
    TokenIndex.h TokenIndex.cpp
    ArtistDictionary.h ArtistDictionary.cpp
    RemotePathParser.h RemotePathParser.cpp
    MatchIndex.h MatchIndex.cpp
    Matcher.h Matcher.cpp
    AlbumMatcher.h AlbumMatcher.cpp
    ReleaseScorer.h ReleaseScorer.cpp
    QueryPlanner.h QueryPlanner.cpp
    Tracklist.h Tracklist.cpp
    DurationConsensus.h DurationConsensus.cpp
    Supersede.h Supersede.cpp
    detail/Alignment.h detail/Alignment.cpp
    detail/Hungarian.h detail/Hungarian.cpp
    detail/IndexData.h
    detail/Scoring.h detail/Scoring.cpp
    detail/RemoteName.h detail/RemoteName.cpp
    detail/Text.h detail/Text.cpp)
add_library(Selecta::Match ALIAS selecta_match)
target_include_directories(selecta_match PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(selecta_match
    PUBLIC Qt6::Core
    PRIVATE rapidfuzz::rapidfuzz)
selecta_set_warnings(selecta_match)
