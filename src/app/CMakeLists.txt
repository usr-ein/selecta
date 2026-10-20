# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(selecta_app STATIC
    AppSettings.h AppSettings.cpp
    Appearance.h Appearance.cpp
    Icons.h Icons.cpp
    Inspector.h Inspector.cpp
    MainWindow.h MainWindow.cpp
    NavDelegate.h NavDelegate.cpp
    NavModel.h NavModel.cpp
    PlayerBar.h PlayerBar.cpp
    Sidebar.h Sidebar.cpp
    SourceListView.h SourceListView.cpp
    models/ActivityModel.h models/ActivityModel.cpp
    views/ActivityView.h views/ActivityView.cpp
    views/EmptyStateView.h views/EmptyStateView.cpp
    views/HistoryView.h views/HistoryView.cpp
    views/InboxView.h views/InboxView.cpp
    views/SettingsView.h views/SettingsView.cpp)
add_library(Selecta::App ALIAS selecta_app)
if(SELECTA_DEVTOOLS)
    target_sources(selecta_app PRIVATE devtools/DebugServer.h devtools/DebugServer.cpp)
    target_compile_definitions(selecta_app PUBLIC SELECTA_DEVTOOLS=1)
endif()
target_link_libraries(selecta_app PUBLIC selecta_base selecta_governor selecta_storage selecta_library selecta_core
    Qt6::Widgets Qt6::Network)
selecta_set_warnings(selecta_app)

# The application bundle.
set(SELECTA_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(NOT SELECTA_MACOS_MINIMUM)
    set(SELECTA_MACOS_MINIMUM "14.4")
endif()
configure_file("${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

qt_add_executable(Selecta MACOSX_BUNDLE main.cpp)
target_link_libraries(Selecta PRIVATE selecta_app)
set_target_properties(Selecta PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
selecta_set_warnings(Selecta)
