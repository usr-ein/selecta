# SPDX-License-Identifier: GPL-3.0-or-later
# playback/: the preview player (head implementer): a decoder thread, a lock-free ring and the audio output
# (CoreAudio's default output on macOS; ADR-003).
qt_add_library(selecta_playback STATIC
    AudioOutput.h AudioOutput.cpp
    Player.h Player.cpp
    RingBuffer.h)
add_library(Selecta::Playback ALIAS selecta_playback)
target_link_libraries(selecta_playback PUBLIC selecta_audio Qt6::Core)
if(APPLE)
    # System frameworks only (ADR-004).
    target_link_libraries(selecta_playback PRIVATE "-framework AudioToolbox" "-framework CoreAudio")
endif()
selecta_set_warnings(selecta_playback)
