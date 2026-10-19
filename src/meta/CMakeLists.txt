# SPDX-License-Identifier: GPL-3.0-or-later
# meta/: metadata and preview services (contract: docs/contracts/meta.md, design: DESIGN.md). Qt Core and Network
# only; the macOS Security framework for the Keychain token store.
qt_add_library(selecta_meta STATIC
    KeychainTokenStore.h KeychainTokenStore.cpp
    MetaCache.h MetaCache.cpp
    Pkce.h Pkce.cpp
    Release.h Release.cpp
    RequestPacer.h RequestPacer.cpp
    Result.h Result.cpp
    ServicePolicy.h ServicePolicy.cpp
    SpotifyAuth.h SpotifyAuth.cpp
    TokenStore.h TokenStore.cpp
    detail/Async.h
    detail/HttpChannel.h detail/HttpChannel.cpp
    detail/Json.h detail/Json.cpp
    detail/LoopbackReceiver.h detail/LoopbackReceiver.cpp
    detail/ServiceGate.h detail/ServiceGate.cpp)
add_library(Selecta::Meta ALIAS selecta_meta)
target_link_libraries(selecta_meta PUBLIC selecta_base selecta_governor Qt6::Core Qt6::Network)
if(APPLE)
    # System frameworks only (ADR-004): SecItem* for the Keychain token store.
    target_link_libraries(selecta_meta PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
selecta_set_warnings(selecta_meta)
