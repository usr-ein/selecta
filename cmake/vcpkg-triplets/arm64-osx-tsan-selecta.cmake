# SPDX-License-Identifier: GPL-3.0-or-later
# The tsan preset's dependencies: arm64-osx-selecta, with Qt built under ThreadSanitizer. Qt then tells TSan about
# its own locks (qtsan_impl.h), the ones in its event queue and thread pool included. Against a prebuilt Qt, every
# payload of a queued cross-thread call or a pooled task looks like a race. Only qtbase needs it; the other ports are
# the same as in arm64-osx-selecta. Repeated rather than included, so a change there changes this triplet's ABI too.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.4)
set(VCPKG_BUILD_TYPE release)
if(PORT STREQUAL "qtbase")
    set(VCPKG_C_FLAGS "-fsanitize=thread")
    set(VCPKG_CXX_FLAGS "-fsanitize=thread")
    set(VCPKG_LINKER_FLAGS "-fsanitize=thread")
endif()
