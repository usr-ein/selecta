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
    internal/CodecHelpers.h)
add_library(Selecta::Slsk ALIAS selecta_slsk)
target_link_libraries(selecta_slsk
    PUBLIC selecta_base selecta_governor Qt6::Core Qt6::Network
    PRIVATE ZLIB::ZLIB)
selecta_set_warnings(selecta_slsk)
