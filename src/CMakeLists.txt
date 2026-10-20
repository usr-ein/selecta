# SPDX-License-Identifier: GPL-3.0-or-later
# Module layout and dependency rules: docs/PLAN.md §2.2 and references/qt-app-architecture.md §12.
# Nothing below app/ may use Qt Widgets.

add_subdirectory(base)
add_subdirectory(governor)
add_subdirectory(storage)

# Libraries with clean boundaries, each built by its own agent against a contract in docs/contracts/ (PLAN §9).
# A library's directory joins the build as soon as it has a CMakeLists.txt.
set(_selecta_libraries slsk meta match)
if(SELECTA_WITH_MEDIA)
    list(APPEND _selecta_libraries audio convert collection)
endif()
foreach(_module IN LISTS _selecta_libraries)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_module}/CMakeLists.txt")
        add_subdirectory(${_module})
    endif()
endforeach()

add_subdirectory(library)
# The engine converts arrivals and the app previews and analyses audio: both need the media libraries.
if(SELECTA_WITH_MEDIA)
    add_subdirectory(core)
    add_subdirectory(playback)
    add_subdirectory(app)
else()
    message(STATUS "SELECTA_WITH_MEDIA is OFF: building the libraries only, not the engine or the app")
endif()
