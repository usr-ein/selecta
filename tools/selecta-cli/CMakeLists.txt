# SPDX-License-Identifier: GPL-3.0-or-later
# selecta-cli: development tools for the local test network and for inspecting a running Selecta.
# It can never reach the live Soulseek network (governor::ServerGuard).
#
# Commands live in commands/<library>/*.cpp and register themselves (Command.h). A library's commands are compiled
# when that library is part of the build, so library agents add commands without editing this file.
qt_add_executable(selecta-cli
    main.cpp
    Command.h Command.cpp
    DebugClient.h DebugClient.cpp)
target_include_directories(selecta-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(selecta-cli PRIVATE selecta_base selecta_governor Qt6::Network)

file(GLOB _selecta_cli_core CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/commands/core/*.cpp")
target_sources(selecta-cli PRIVATE ${_selecta_cli_core})

foreach(_library slsk meta match audio convert collection share)
    if(TARGET selecta_${_library})
        file(GLOB _selecta_cli_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/commands/${_library}/*.cpp")
        target_sources(selecta-cli PRIVATE ${_selecta_cli_sources})
        target_link_libraries(selecta-cli PRIVATE selecta_${_library})
    endif()
endforeach()

selecta_set_warnings(selecta-cli)
