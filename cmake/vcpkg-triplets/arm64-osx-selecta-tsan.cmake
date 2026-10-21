# SPDX-License-Identifier: GPL-3.0-or-later
# The tsan preset's dependencies: arm64-osx-selecta, with Qt Base built with ThreadSanitizer. An uninstrumented Qt hides
# its own synchronisation (the event queue, the thread pool, futures) from TSan, which then reports every value handed
# from one thread to another through Qt as a race. The other dependencies don't hand data between threads.
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
