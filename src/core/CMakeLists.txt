# SPDX-License-Identifier: GPL-3.0-or-later
# core/: services on the engine thread; wires the governor, storage and the libraries together (head implementer).
qt_add_library(selecta_core STATIC
    ActivityLog.h ActivityLog.cpp
    Engine.h Engine.cpp
    EngineHost.h EngineHost.cpp
    GovernorStore.h GovernorStore.cpp)
add_library(Selecta::Core ALIAS selecta_core)
target_link_libraries(selecta_core PUBLIC selecta_base selecta_governor selecta_storage selecta_library Qt6::Core)
selecta_set_warnings(selecta_core)
