# SPDX-License-Identifier: GPL-3.0-or-later
# core/: services on the engine thread; wires the governor, storage and the libraries together (head implementer).
qt_add_library(selecta_core STATIC
    ActivityLog.h ActivityLog.cpp
    Engine.h Engine.cpp
    FileRanking.h FileRanking.cpp
    EngineHost.h EngineHost.cpp
    GetTracks.h GetTracks.cpp
    GetTracksService.h GetTracksService.cpp
    GovernorPacer.h GovernorPacer.cpp
    GovernorStore.h GovernorStore.cpp
    InboxService.h InboxService.cpp
    MetaServices.h MetaServices.cpp
    NetworkService.h NetworkService.cpp
    ReleaseJson.h ReleaseJson.cpp
    SearchService.h SearchService.cpp)
add_library(Selecta::Core ALIAS selecta_core)
target_link_libraries(selecta_core PUBLIC selecta_base selecta_governor selecta_storage selecta_library selecta_match selecta_meta selecta_slsk
    Qt6::Core Qt6::Network)
selecta_set_warnings(selecta_core)
