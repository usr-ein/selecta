# SPDX-License-Identifier: GPL-3.0-or-later
# core/: services on the engine thread; wires the governor, storage and the libraries together (head implementer).
qt_add_library(selecta_core STATIC
    ActivityLog.h ActivityLog.cpp
    AlbumArrival.h AlbumArrival.cpp
    AlbumService.h AlbumService.cpp
    BrowseService.h BrowseService.cpp
    Albums.h Albums.cpp
    ArrivalService.h ArrivalService.cpp
    ClipPreviews.h ClipPreviews.cpp
    CollectionBrowse.h CollectionBrowse.cpp
    CollectionService.h CollectionService.cpp
    CollectionStore.h CollectionStore.cpp
    CountryDatabase.h CountryDatabase.cpp
    CountryDatabaseFetcher.h CountryDatabaseFetcher.cpp
    ConverterService.h ConverterService.cpp
    Downloads.h Downloads.cpp
    Engine.h Engine.cpp
    FileRanking.h FileRanking.cpp
    EngineHost.h EngineHost.cpp
    GetTracks.h GetTracks.cpp
    GetTracksService.h GetTracksService.cpp
    GovernorPacer.h GovernorPacer.cpp
    GovernorStore.h GovernorStore.cpp
    InboxConversion.h InboxConversion.cpp
    InboxRetag.h InboxRetag.cpp
    InboxService.h InboxService.cpp
    ManualSearchService.h ManualSearchService.cpp
    MatchCorrections.h MatchCorrections.cpp
    MetaServices.h MetaServices.cpp
    NetworkService.h NetworkService.cpp
    OwnRelease.h OwnRelease.cpp
    PeerStats.h PeerStats.cpp
    PortMapClient.h PortMapClient.cpp
    PortMapper.h PortMapper.cpp
    PreviewCache.h PreviewCache.cpp
    ReleaseJson.h ReleaseJson.cpp
    ResultAnnotator.h ResultAnnotator.cpp
    RoomService.h RoomService.cpp
    SearchCache.h SearchCache.cpp
    SearchFilter.h SearchFilter.cpp
    SearchResults.h SearchResults.cpp
    SearchService.h SearchService.cpp
    SocialService.h SocialService.cpp
    SpectrumCheck.h SpectrumCheck.cpp
    ShareService.h ShareService.cpp
    TransferService.h TransferService.cpp
    WishlistService.h WishlistService.cpp
    YouTubePreviews.h YouTubePreviews.cpp)
add_library(Selecta::Core ALIAS selecta_core)
target_link_libraries(selecta_core PUBLIC selecta_base selecta_governor selecta_storage selecta_library selecta_match
    selecta_meta selecta_slsk selecta_audio selecta_convert selecta_collection selecta_share Qt6::Core Qt6::Network)
# libnatpmp (vcpkg, static; no CMake package): only its default-gateway lookup, for NAT-PMP (NET-3).
find_path(SELECTA_NATPMP_INCLUDE_DIR natpmp.h REQUIRED)
find_library(SELECTA_NATPMP_LIBRARY natpmp REQUIRED)
target_include_directories(selecta_core SYSTEM PRIVATE "${SELECTA_NATPMP_INCLUDE_DIR}")
target_link_libraries(selecta_core PRIVATE "${SELECTA_NATPMP_LIBRARY}")
# libmaxminddb (vcpkg, static): reads the DB-IP Lite country database for peers' flags (P-002).
find_package(maxminddb CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(selecta_core PRIVATE maxminddb::maxminddb ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(selecta_core PRIVATE ws2_32 iphlpapi)
endif()
selecta_set_warnings(selecta_core)
