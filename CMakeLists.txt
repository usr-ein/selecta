# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

# Options that decide which vcpkg features to install must exist before project().
option(SELECTA_BUILD_TESTS "Build the test suites" ON)
option(SELECTA_WITH_MEDIA "Build with FFmpeg, LAME, soxr and TagLib (audio, waveforms, conversion)" ON)
if(SELECTA_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
if(SELECTA_WITH_MEDIA)
    list(APPEND VCPKG_MANIFEST_FEATURES "media")
endif()

project(Selecta
    VERSION 0.1.0
    DESCRIPTION "A quality-first Soulseek client for DJs and collectors"
    HOMEPAGE_URL "https://github.com/usr-ein/selecta"
    LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SELECTA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SELECTA_DEVTOOLS "Build the developer tools (debug socket for selecta-cli app)" ON)
option(SELECTA_CLANG_TIDY "Run clang-tidy on Selecta's own sources while compiling" OFF)
# PRD SAFE-3: a build can reach the live Soulseek server only when this is ON *and* the user enables it at run time.
option(SELECTA_ENABLE_LIVE_SERVER "Allow connecting to the live Soulseek server" OFF)
set(SELECTA_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address;undefined or thread")
include(SelectaCcache)

if(SELECTA_BUILD_TESTS AND SELECTA_ENABLE_LIVE_SERVER)
    message(FATAL_ERROR "Tests must never be built with SELECTA_ENABLE_LIVE_SERVER (CLAUDE.md network safety rules)")
endif()

if(APPLE)
    # vcpkg searches libraries before frameworks, so FindOpenGL (pulled in by Qt Gui) would pick XQuartz's libGL from
    # /usr/X11R6 when XQuartz is installed, and Qt would then drop the link. Point it at the SDK framework first,
    # replacing any non-framework value already in the cache.
    if(NOT OPENGL_gl_LIBRARY MATCHES "/OpenGL\\.framework$")
        unset(OPENGL_gl_LIBRARY CACHE)
    endif()
    if(NOT OPENGL_INCLUDE_DIR MATCHES "/OpenGL\\.framework")
        unset(OPENGL_INCLUDE_DIR CACHE)
    endif()
    set(_selecta_frameworks "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks")
    find_library(OPENGL_gl_LIBRARY NAMES OpenGL PATHS "${_selecta_frameworks}" NO_DEFAULT_PATH REQUIRED)
    set(OPENGL_INCLUDE_DIR "${_selecta_frameworks}/OpenGL.framework/Headers" CACHE PATH "OpenGL headers" FORCE)
endif()

# Qt 6.12 is the target (ADR-004); vcpkg's baseline currently provides 6.11.
set(_selecta_qt_components Core Gui Widgets Network)
if(SELECTA_BUILD_TESTS)
    list(APPEND _selecta_qt_components Test)
endif()
find_package(Qt6 6.11 REQUIRED COMPONENTS ${_selecta_qt_components})
qt_standard_project_setup(REQUIRES 6.11)
add_compile_definitions(QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_URL_CAST_FROM_STRING
    QT_NO_CAST_FROM_BYTEARRAY QT_USE_QSTRINGBUILDER QT_NO_FOREACH)

get_target_property(_selecta_qt_core_type Qt6::Core TYPE)
if(_selecta_qt_core_type STREQUAL "STATIC_LIBRARY")
    set(SELECTA_STATIC_QT ON)
else()
    set(SELECTA_STATIC_QT OFF)
    message(WARNING "Qt is a shared library: Selecta is meant to link everything statically (ADR-004). "
                    "Use a vcpkg preset (see docs/BUILDING.md).")
endif()

include(SelectaWarnings)
include(SelectaSanitizers)
include(SelectaDependencies)

add_subdirectory(src)
add_subdirectory(tools/selecta-cli)

if(SELECTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
