# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the slsk library. They never touch the real network: codec tests are pure, session tests use in-process
# fakes on loopback (fixtures/), and the Soulfind integration tests (label `devenv`) run only with SELECTA_DEVENV=1.

# The fixtures as a library, so core/'s tests can run end to end against the same fakes:
#   FakeServer, FakePeer  scriptable in-process server and peer on loopback
#   TestPermits           a PermitSource that grants, refuses or holds PeerLookup permits
#   Samples               a filled-in instance of every wire message
#   TestUtil              waitFor() / settle() on the event loop, permit()
# Include as "fixtures/FakeServer.h" etc. Test-only: never linked into the app.
qt_add_library(selecta_slsk_testing STATIC
    fixtures/FakeServer.h fixtures/FakeServer.cpp
    fixtures/FakePeer.h fixtures/FakePeer.cpp
    fixtures/Samples.h fixtures/Samples.cpp
    fixtures/TestPermits.h
    fixtures/TestUtil.h)
target_include_directories(selecta_slsk_testing PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests") # support/PermitFactory.h
target_link_libraries(selecta_slsk_testing PUBLIC selecta_slsk Qt6::Network Qt6::Test)
selecta_set_warnings(selecta_slsk_testing)

# selecta-devnet, the local network for manual runs, is built on these fixtures.
add_subdirectory("${PROJECT_SOURCE_DIR}/tools/devnet" "${CMAKE_BINARY_DIR}/tools/devnet")

selecta_add_test_suite(selecta_slsk_tests
    SOURCES
        CodecTest.cpp
        ZlibTest.cpp
        TypesTest.cpp
        FrameTest.cpp
        ServerMessagesTest.cpp
        PeerMessagesTest.cpp
        DistributedMessagesTest.cpp
        ServerSessionTest.cpp
        PeerNetworkTest.cpp
        TransferManagerTest.cpp
        DistributedNodeTest.cpp
        ClientTest.cpp
        DevnetTest.cpp
    LIBRARIES selecta_slsk_testing selecta_devnet_core)

# slsk against a real Soulfind on this machine (devenv/README.md). Soulfind listens on every interface, so these are
# opt-in twice: built only with SELECTA_SOULFIND_TESTS=ON (CTest label `devenv`), and run only with SELECTA_DEVENV=1.
# Nothing here downloads Soulfind: fetch the pinned build by hand (devenv/fetch-soulfind.sh).
option(SELECTA_SOULFIND_TESTS "Build the slsk tests against a real Soulfind (it listens on every interface)" OFF)
if(SELECTA_SOULFIND_TESTS)
    add_executable(selecta_slsk_devenv_tests "${PROJECT_SOURCE_DIR}/tests/unit_main.cpp" devenv/SoulfindTest.cpp)
    target_link_libraries(selecta_slsk_devenv_tests PRIVATE selecta_slsk_testing Qt6::Core Qt6::Test GTest::gtest)
    target_compile_definitions(selecta_slsk_devenv_tests PRIVATE SELECTA_DEVENV_DIR="${PROJECT_SOURCE_DIR}/devenv")
    selecta_set_warnings(selecta_slsk_devenv_tests)
    gtest_discover_tests(selecta_slsk_devenv_tests DISCOVERY_MODE PRE_TEST PROPERTIES LABELS devenv)
endif()

if(SELECTA_SLSK_FUZZ)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/fuzz/slsk" "${CMAKE_BINARY_DIR}/tests/fuzz/slsk")
endif()
