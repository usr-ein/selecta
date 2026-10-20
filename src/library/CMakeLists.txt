# SPDX-License-Identifier: GPL-3.0-or-later
# library/: the Inbox, filing, the collection index and supersede (head implementer; ADR-009).
qt_add_library(selecta_library STATIC
    FileOps.h FileOps.cpp
    HistoryStore.h HistoryStore.cpp
    InboxStore.h InboxStore.cpp
    Naming.h Naming.cpp
    Quarantine.h Quarantine.cpp)
add_library(Selecta::Library ALIAS selecta_library)
target_link_libraries(selecta_library PUBLIC selecta_base selecta_storage Qt6::Core)
selecta_set_warnings(selecta_library)
