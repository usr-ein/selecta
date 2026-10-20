# SPDX-License-Identifier: GPL-3.0-or-later
# Linux in CI (NFR-1): every dependency a static library, as on macOS (ADR-004); glibc, libstdc++ and the system's
# OpenGL stay dynamic. Dependencies are built in release mode only: Selecta's own debug builds link them fine.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
