# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for playback/ (head implementer). Audio goes to a silent or hand-driven output; test audio is synthetic.
selecta_add_test_suite(selecta_playback_tests
    SOURCES
        PlayerTest.cpp
        RingBufferTest.cpp
    LIBRARIES selecta_playback selecta_media_test_support)
