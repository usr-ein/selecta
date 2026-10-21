# SPDX-License-Identifier: GPL-3.0-or-later
# collection/: the collection scanner (docs/contracts/collection.md). Reads the user's music folders, read-only:
# tags through convert/, stream facts through audio/, names through match/. No database, no network.

qt_add_library(selecta_collection STATIC
    Scanner.h Scanner.cpp
    Albums.h Albums.cpp
    FolderRoles.h FolderRoles.cpp
    Watcher.h Watcher.cpp
    detail/FileReader.h detail/FileReader.cpp
    detail/Walk.h detail/Walk.cpp)
add_library(Selecta::Collection ALIAS selecta_collection)
target_include_directories(selecta_collection PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(selecta_collection PUBLIC selecta_audio selecta_convert selecta_match Qt6::Core)
if(APPLE)
    # FSEvents (Watcher): a system framework.
    target_link_libraries(selecta_collection PRIVATE "-framework CoreServices")
endif()
selecta_set_warnings(selecta_collection)
