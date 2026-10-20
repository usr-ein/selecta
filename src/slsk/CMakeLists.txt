# SPDX-License-Identifier: GPL-3.0-or-later
# slsk: the Soulseek protocol library (docs/contracts/slsk.md, DESIGN.md). Depends only on Qt Core, Qt Network, zlib,
# selecta_base and selecta_governor; no Widgets, no Gui.
find_package(ZLIB REQUIRED)

qt_add_library(selecta_slsk STATIC
    Protocol.h
    Codec.h Codec.cpp
    Zlib.h Zlib.cpp
    Types.h Types.cpp
    ServerMessages.h ServerMessages.cpp
    PeerMessages.h PeerMessages.cpp
    DistributedMessages.h DistributedMessages.cpp
    Frame.h Frame.cpp
    TokenGenerator.h TokenGenerator.cpp
    Logging.h Logging.cpp
    internal/CodecHelpers.h
    ServerSession.h ServerSession.cpp
    PermitSource.h
    Providers.h
    PeerNetwork.h PeerNetwork.cpp
    MetaTypes.h MetaTypes.cpp
    TransferManager.h TransferManager.cpp)
add_library(Selecta::Slsk ALIAS selecta_slsk)
target_link_libraries(selecta_slsk
    PUBLIC selecta_base selecta_governor Qt6::Core Qt6::Network
    PRIVATE ZLIB::ZLIB)
selecta_set_warnings(selecta_slsk)

# The fuzz configuration (tests/fuzz/slsk/README.md): coverage instrumentation for libFuzzer. Needs LLVM clang.
option(SELECTA_SLSK_FUZZ "Build the slsk libFuzzer targets (LLVM clang; tests/fuzz/slsk/README.md)" OFF)
if(SELECTA_SLSK_FUZZ)
    target_compile_options(selecta_slsk PRIVATE -fsanitize=fuzzer-no-link)
endif()
