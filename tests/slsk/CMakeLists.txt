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
        DevnetTest.cpp
    LIBRARIES selecta_slsk_testing selecta_devnet_core)

if(SELECTA_SLSK_FUZZ)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/fuzz/slsk" "${CMAKE_BINARY_DIR}/tests/fuzz/slsk")
endif()
