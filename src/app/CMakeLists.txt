# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(selecta_app STATIC
    Accessibility.h Accessibility.cpp
    AppSettings.h AppSettings.cpp
    Appearance.h Appearance.cpp
    Diagnostics.h Diagnostics.cpp
    Format.h Format.cpp
    Icons.h Icons.cpp
    Inspector.h Inspector.cpp
    MeterDelegate.h MeterDelegate.cpp
    MainWindow.h MainWindow.cpp
    NavDelegate.h NavDelegate.cpp
    NavModel.h NavModel.cpp
    Notifications.h Notifications.cpp
    PeerMenu.h PeerMenu.cpp
    PlayerBar.h PlayerBar.cpp
    Presets.h Presets.cpp
    Preview.h Preview.cpp
    Sidebar.h Sidebar.cpp
    SourceListView.h SourceListView.cpp
    SpectrogramView.h SpectrogramView.cpp
    TickDelegate.h TickDelegate.cpp
    Toasts.h Toasts.cpp
    TwoLineDelegate.h TwoLineDelegate.cpp
    WaveformView.h WaveformView.cpp
    models/ActivityModel.h models/ActivityModel.cpp
    models/BrowseTreeModel.h models/BrowseTreeModel.cpp
    models/ConverterModel.h models/ConverterModel.cpp
    models/GetTracksModel.h models/GetTracksModel.cpp
    models/PeersModel.h models/PeersModel.cpp
    models/SearchResultsModel.h models/SearchResultsModel.cpp
    models/TransfersModel.h models/TransfersModel.cpp
    views/ActivityView.h views/ActivityView.cpp
    views/AddTracksDialog.h views/AddTracksDialog.cpp
    views/BrowseView.h views/BrowseView.cpp
    views/CollectionView.h views/CollectionView.cpp
    views/ConverterInspector.h views/ConverterInspector.cpp
    views/ConverterView.h views/ConverterView.cpp
    views/EditTagsDialog.h views/EditTagsDialog.cpp
    views/EmptyStateView.h views/EmptyStateView.cpp
    views/GetTracksInspector.h views/GetTracksInspector.cpp
    views/GetTracksView.h views/GetTracksView.cpp
    views/HistoryView.h views/HistoryView.cpp
    views/InboxInspector.h views/InboxInspector.cpp
    views/InboxView.h views/InboxView.cpp
    views/MessagesView.h views/MessagesView.cpp
    views/ReleasePickerDialog.h views/ReleasePickerDialog.cpp
    views/RoomsView.h views/RoomsView.cpp
    views/SearchInspector.h views/SearchInspector.cpp
    views/SearchView.h views/SearchView.cpp
    views/SettingsView.h views/SettingsView.cpp
    views/SharesView.h views/SharesView.cpp
    views/WishlistView.h views/WishlistView.cpp)
add_library(Selecta::App ALIAS selecta_app)
if(APPLE)
    # System notifications (UI-6): UNUserNotificationCenter, in Objective-C++ with ARC. System frameworks only.
    # The system accent colour in light and dark (Appearance.h), from AppKit.
    target_sources(selecta_app PRIVATE MacNotifications.mm AppearanceMac.mm)
    set_source_files_properties(MacNotifications.mm AppearanceMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(selecta_app PRIVATE "-framework UserNotifications" "-framework AppKit"
        "-framework Foundation")
endif()
if(SELECTA_DEVTOOLS)
    target_sources(selecta_app PRIVATE devtools/DebugServer.h devtools/DebugServer.cpp)
    target_compile_definitions(selecta_app PUBLIC SELECTA_DEVTOOLS=1)
endif()
target_link_libraries(selecta_app PUBLIC selecta_base selecta_governor selecta_storage selecta_library selecta_core
    selecta_playback Qt6::Widgets Qt6::Network)
selecta_set_warnings(selecta_app)
# The window icon on Linux and Windows (icons::applicationIcon), drawn by packaging/logo/make-icons.sh.
set(_selecta_icons "${PROJECT_SOURCE_DIR}/packaging/icons")
qt_add_resources(selecta_app "app_icon" PREFIX "/icons" BASE "${_selecta_icons}" FILES
    "${_selecta_icons}/selecta-16.png" "${_selecta_icons}/selecta-24.png" "${_selecta_icons}/selecta-32.png"
    "${_selecta_icons}/selecta-48.png" "${_selecta_icons}/selecta-64.png" "${_selecta_icons}/selecta-128.png"
    "${_selecta_icons}/selecta-256.png")

# The application bundle.
set(SELECTA_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(NOT SELECTA_MACOS_MINIMUM)
    set(SELECTA_MACOS_MINIMUM "14.4")
endif()

# Its icon (D36), drawn by packaging/logo/make-icons.sh: the round record, Selecta.icns. macOS 26 and later draw every
# app icon in their rounded square and put a round one on a grey tile, so when actool from Xcode 26 or later is there,
# it also compiles the Icon Composer document (the record on a dark tile) into Assets.car. CFBundleIconName names it,
# and macOS prefers it to the .icns from version 14 on (14 and 15 show actool's flat rendering of the tile). Without
# that actool (Command Line Tools only, an older Xcode), or configured with -DSELECTA_ACTOOL=, the bundle has the .icns
# alone.
set(SELECTA_BUNDLE_ICON_NAME "")
if(APPLE)
    function(selecta_accept_actool result candidate)
        execute_process(COMMAND "${candidate}" --version OUTPUT_VARIABLE version RESULT_VARIABLE status ERROR_QUIET
            TIMEOUT 120)
        if(status EQUAL 0 AND version MATCHES "<key>short-bundle-version</key>[ \t\r\n]*<string>([0-9]+)")
            if(CMAKE_MATCH_1 GREATER_EQUAL 26)
                return()
            endif()
        endif()
        set(${result} FALSE PARENT_SCOPE)
    endfunction()
    find_program(SELECTA_ACTOOL actool VALIDATOR selecta_accept_actool
        DOC "actool from Xcode 26 or later, which compiles the Icon Composer app icon")
    if(SELECTA_ACTOOL)
        set(SELECTA_BUNDLE_ICON_NAME "<key>CFBundleIconName</key><string>Selecta</string>")
        message(STATUS "App icon: Selecta.icns, and Selecta.icon compiled by ${SELECTA_ACTOOL}")
    else()
        message(STATUS "App icon: Selecta.icns only (no actool from Xcode 26 or later)")
    endif()
endif()
configure_file("${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

qt_add_executable(Selecta MACOSX_BUNDLE main.cpp)
target_link_libraries(Selecta PRIVATE selecta_app)
set_target_properties(Selecta PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    MACOSX_BUNDLE_ICON_FILE Selecta.icns)
selecta_set_warnings(Selecta)
if(APPLE)
    set(_selecta_icns "${PROJECT_SOURCE_DIR}/packaging/macos/Selecta.icns")
    target_sources(Selecta PRIVATE "${_selecta_icns}")
    set_source_files_properties("${_selecta_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    if(SELECTA_ACTOOL)
        # actool writes its own Selecta.icns (the flat tile) beside Assets.car; the bundle keeps the round one.
        set(_selecta_icon "${PROJECT_SOURCE_DIR}/packaging/macos/Selecta.icon")
        set(_selecta_actool_out "${CMAKE_CURRENT_BINARY_DIR}/actool")
        add_custom_command(OUTPUT "${_selecta_actool_out}/Assets.car"
            BYPRODUCTS "${_selecta_actool_out}/Selecta.icns" "${_selecta_actool_out}/partial.plist"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_selecta_actool_out}"
            COMMAND "${SELECTA_ACTOOL}" "${_selecta_icon}" --compile "${_selecta_actool_out}" --platform macosx
                --target-device mac --minimum-deployment-target "${SELECTA_MACOS_MINIMUM}" --app-icon Selecta
                --output-partial-info-plist "${_selecta_actool_out}/partial.plist" --errors --warnings
                --output-format human-readable-text
            DEPENDS "${_selecta_icon}/icon.json" "${_selecta_icon}/Assets/disc.png"
            COMMENT "Compiling the Icon Composer app icon"
            VERBATIM)
        target_sources(Selecta PRIVATE "${_selecta_actool_out}/Assets.car")
        set_source_files_properties("${_selecta_actool_out}/Assets.car" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
endif()
