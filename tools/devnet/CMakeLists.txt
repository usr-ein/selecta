# SPDX-License-Identifier: GPL-3.0-or-later
# selecta-devnet: a local Soulseek network (FakeServer and FakePeers on 127.0.0.1) for running Selecta by hand.
# A development tool built on the slsk test fixtures, so it is configured with the tests (tests/slsk/CMakeLists.txt)
# and never shipped. Usage: selecta-devnet --help, and the header of main.cpp.
qt_add_library(selecta_devnet_core STATIC
    AudioProbe.h AudioProbe.cpp
    Behaviour.h Behaviour.cpp
    Corpus.h Corpus.cpp
    Describe.h Describe.cpp
    Library.h Library.cpp
    Network.h Network.cpp)
target_include_directories(selecta_devnet_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(selecta_devnet_core PUBLIC selecta_slsk_testing selecta_slsk Qt6::Core Qt6::Network)
selecta_set_warnings(selecta_devnet_core)

qt_add_executable(selecta-devnet main.cpp)
target_link_libraries(selecta-devnet PRIVATE selecta_devnet_core)
selecta_set_warnings(selecta-devnet)
