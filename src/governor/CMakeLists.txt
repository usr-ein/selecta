# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(selecta_governor STATIC
    Limits.h
    ServerGuard.h ServerGuard.cpp
    TokenBucket.h TokenBucket.cpp)
add_library(Selecta::Governor ALIAS selecta_governor)
target_link_libraries(selecta_governor PUBLIC selecta_base Qt6::Network)
if(SELECTA_ENABLE_LIVE_SERVER)
    target_compile_definitions(selecta_governor PRIVATE SELECTA_LIVE_SERVER_COMPILED_IN=1)
endif()
selecta_set_warnings(selecta_governor)
