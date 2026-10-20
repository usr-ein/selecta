# SPDX-License-Identifier: GPL-3.0-or-later
# meta/: metadata and preview services (contract: docs/contracts/meta.md, design: DESIGN.md). Qt Core and Network,
# match/ for names and versions; the macOS Security framework for the Keychain token store.
qt_add_library(selecta_meta STATIC
    CoverFetcher.h CoverFetcher.cpp
    DeezerClient.h DeezerClient.cpp
    KeychainTokenStore.h KeychainTokenStore.cpp
    MetaCache.h MetaCache.cpp
    MusicBrainzClient.h MusicBrainzClient.cpp
    Pkce.h Pkce.cpp
    Priority.h
    Release.h Release.cpp
    ReleaseResolver.h ReleaseResolver.cpp
    RequestPacer.h RequestPacer.cpp
    Result.h Result.cpp
    ServicePolicy.h ServicePolicy.cpp
    SpotifyAuth.h SpotifyAuth.cpp
    SpotifyClient.h SpotifyClient.cpp
    SpotifyLinks.h SpotifyLinks.cpp
    TokenStore.h TokenStore.cpp
    detail/Async.h
    detail/DeezerJson.h detail/DeezerJson.cpp
    detail/HttpChannel.h detail/HttpChannel.cpp
    detail/Identifiers.h detail/Identifiers.cpp
    detail/ImageHeader.h detail/ImageHeader.cpp
    detail/Json.h detail/Json.cpp
    detail/LoopbackReceiver.h detail/LoopbackReceiver.cpp
    detail/MusicBrainzJson.h detail/MusicBrainzJson.cpp
    detail/ServiceGate.h detail/ServiceGate.cpp
    detail/SpotifyJson.h detail/SpotifyJson.cpp
    detail/TitleInfo.h detail/TitleInfo.cpp
    detail/Worker.h detail/Worker.cpp)
add_library(Selecta::Meta ALIAS selecta_meta)
target_link_libraries(selecta_meta PUBLIC selecta_base selecta_governor selecta_match Qt6::Core Qt6::Network)
if(APPLE)
    # System frameworks only (ADR-004): SecItem* for the Keychain token store.
    target_link_libraries(selecta_meta PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
selecta_set_warnings(selecta_meta)
