# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for audio/. Test audio is synthetic, generated at test time (support/SyntheticAudio).

find_package(mp3lame CONFIG REQUIRED)

# Synthetic files and decode helpers, shared with tests/convert/.
add_library(selecta_media_test_support STATIC
    support/SyntheticAudio.h support/SyntheticAudio.cpp
    support/DecodeHelpers.h support/DecodeHelpers.cpp)
target_include_directories(selecta_media_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(selecta_media_test_support PUBLIC selecta_audio Qt6::Core mp3lame::mp3lame)
selecta_set_warnings(selecta_media_test_support)

selecta_add_test_suite(selecta_audio_tests
    SOURCES
        AvioAdapterTest.cpp
        ByteSourceTest.cpp
        DecoderTest.cpp
        ParserTest.cpp
    LIBRARIES selecta_audio selecta_media_test_support)
