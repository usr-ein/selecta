# SPDX-License-Identifier: GPL-3.0-or-later
# meta/ tests. Nothing here touches a real service: HTTP goes to FakeNetwork (scripted replies), the sign-in callback
# to a loopback listener in the same process, and yt-dlp to the fake script in fixtures/.
selecta_add_test_suite(selecta_meta_tests
    SOURCES
        support/Await.h
        support/FakeNetwork.h support/FakeNetwork.cpp
        support/FakePacer.h
        HttpChannelTest.cpp
        JsonTest.cpp
        LoopbackReceiverTest.cpp
        MetaCacheTest.cpp
        PkceTest.cpp
        ResultTest.cpp
        ServiceGateTest.cpp
        SpotifyAuthTest.cpp
        TokenStoreTest.cpp
    LIBRARIES selecta_meta Qt6::Network)
target_compile_definitions(selecta_meta_tests PRIVATE
    SELECTA_META_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
