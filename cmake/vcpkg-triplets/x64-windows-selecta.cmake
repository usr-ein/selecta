# SPDX-License-Identifier: GPL-3.0-or-later
# Windows in CI (NFR-1): every dependency a static library (ADR-004) on the dynamic CRT (/MD). Release builds only,
# so Selecta builds as RelWithDebInfo on Windows: MSVC can't mix the debug and release CRTs.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
