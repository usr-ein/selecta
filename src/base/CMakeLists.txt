# SPDX-License-Identifier: GPL-3.0-or-later
configure_file(Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/base/Version.h" @ONLY)

qt_add_library(selecta_base STATIC
    Clock.h Clock.cpp
    Logging.h Logging.cpp
    Paths.h Paths.cpp)
add_library(Selecta::Base ALIAS selecta_base)
target_include_directories(selecta_base PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(selecta_base PUBLIC Qt6::Core)
selecta_set_warnings(selecta_base)
