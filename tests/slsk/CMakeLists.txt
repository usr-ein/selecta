# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the slsk library. They never touch the real network: codec tests are pure, session tests use in-process
# fakes on loopback (fixtures/), and the Soulfind integration tests (label `devenv`) run only with SELECTA_DEVENV=1.
selecta_add_test_suite(selecta_slsk_tests
    SOURCES
        CodecTest.cpp
        ZlibTest.cpp
        TypesTest.cpp
        FrameTest.cpp
        ServerMessagesTest.cpp
        PeerMessagesTest.cpp
        DistributedMessagesTest.cpp
        fixtures/Samples.h fixtures/Samples.cpp
    LIBRARIES selecta_slsk)
target_include_directories(selecta_slsk_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(SELECTA_SLSK_FUZZ)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/fuzz/slsk" "${CMAKE_BINARY_DIR}/tests/fuzz/slsk")
endif()
