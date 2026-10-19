# SPDX-License-Identifier: GPL-3.0-or-later
# Headless tool for the local test network and for inspecting a running Selecta (`selecta-cli app …`).
# It can never reach the live Soulseek network (ServerGuard).
qt_add_executable(selecta-cli
    main.cpp
    DebugClient.h DebugClient.cpp)
target_link_libraries(selecta-cli PRIVATE selecta_base selecta_governor Qt6::Network)
selecta_set_warnings(selecta-cli)
