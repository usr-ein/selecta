# SPDX-License-Identifier: GPL-3.0-or-later
# Dependencies fetched at configure time, pinned by URL and SHA-256 (ADR-004, ADR-005).

include(FetchContent)

# SQLite: Selecta links its own copy so behaviour doesn't depend on the Qt build or the OS (ADR-005).
FetchContent_Declare(selecta_sqlite_src
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(selecta_sqlite_src)

add_library(selecta_sqlite3 STATIC "${selecta_sqlite_src_SOURCE_DIR}/sqlite3.c")
add_library(Selecta::SQLite3 ALIAS selecta_sqlite3)
target_include_directories(selecta_sqlite3 SYSTEM PUBLIC "${selecta_sqlite_src_SOURCE_DIR}")
# Recommended options from https://sqlite.org/compile.html; one connection per thread (THREADSAFE=2).
target_compile_definitions(selecta_sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_USE_ALLOCA
    SQLITE_ENABLE_FTS5
    SQLITE_STRICT_SUBTYPE=1)
set_target_properties(selecta_sqlite3 PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(selecta_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

if(SELECTA_BUILD_TESTS)
    # Sanitizer builds compile GoogleTest from source with the same instrumentation (SelectaSanitizers.cmake applies
    # to every target defined after it). A prebuilt, uninstrumented GoogleTest makes libc++'s container annotations
    # disagree between the two sides, and ASan then reports false overflows inside GoogleTest's own registration.
    if(SELECTA_SANITIZE)
        set(_selecta_gtest_find_args "")
    else()
        set(_selecta_gtest_find_args FIND_PACKAGE_ARGS NAMES GTest)
    endif()
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        ${_selecta_gtest_find_args})
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
