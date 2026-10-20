# SPDX-License-Identifier: GPL-3.0-or-later
# Name matching and ranking (docs/contracts/match.md, ADR-008). Pure logic on Qt Core and rapidfuzz-cpp (MIT,
# header-only, from vcpkg): no I/O, no database, no network, no UI.
find_package(rapidfuzz CONFIG REQUIRED)

qt_add_library(selecta_match STATIC
    Types.h Types.cpp
    Lexicon.h Lexicon.cpp
    Normalizer.h Normalizer.cpp
    VersionClassifier.h VersionClassifier.cpp
    Similarity.h Similarity.cpp
    TokenIndex.h TokenIndex.cpp
    ArtistDictionary.h ArtistDictionary.cpp
    RemotePathParser.h RemotePathParser.cpp
    Tracklist.h Tracklist.cpp
    detail/Alignment.h detail/Alignment.cpp
    detail/RemoteName.h detail/RemoteName.cpp
    detail/Text.h detail/Text.cpp)
add_library(Selecta::Match ALIAS selecta_match)
target_include_directories(selecta_match PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(selecta_match
    PUBLIC Qt6::Core
    PRIVATE rapidfuzz::rapidfuzz)
selecta_set_warnings(selecta_match)
