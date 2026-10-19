# SPDX-License-Identifier: GPL-3.0-or-later
# Every dependency is a static library (ADR-004); only macOS system frameworks and libc++ are linked dynamically.
# Dependencies are built in release mode only: Selecta's own debug builds link them fine on macOS.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.4)
set(VCPKG_BUILD_TYPE release)
