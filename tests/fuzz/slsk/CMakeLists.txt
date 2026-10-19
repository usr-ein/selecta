# SPDX-License-Identifier: GPL-3.0-or-later
# libFuzzer targets for the slsk decoders, one per family. Built only in the fuzz configuration (-DSELECTA_SLSK_FUZZ=ON)
# with an LLVM clang that ships libFuzzer; Apple clang has none. See README.md and fuzz.sh in this directory.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "SELECTA_SLSK_FUZZ needs LLVM clang with libFuzzer (Homebrew llvm on macOS; Apple clang has "
                        "none). See tests/fuzz/slsk/README.md.")
endif()

set(_slsk_fuzz_targets
    server ServerFuzz.cpp
    peer_init PeerInitFuzz.cpp
    peer PeerFuzz.cpp
    distrib DistribFuzz.cpp
    zlib ZlibFuzz.cpp)
while(_slsk_fuzz_targets)
    list(POP_FRONT _slsk_fuzz_targets _name _source)
    add_executable(slsk_fuzz_${_name} ${_source} FuzzCommon.h)
    target_link_libraries(slsk_fuzz_${_name} PRIVATE selecta_slsk)
    target_compile_options(slsk_fuzz_${_name} PRIVATE -fsanitize=fuzzer)
    target_link_options(slsk_fuzz_${_name} PRIVATE -fsanitize=fuzzer)
    selecta_set_warnings(slsk_fuzz_${_name})
endwhile()

# Writes the seed corpus from the unit-test samples.
add_executable(slsk_fuzz_seeds SeedWriter.cpp "${PROJECT_SOURCE_DIR}/tests/slsk/fixtures/Samples.cpp")
target_include_directories(slsk_fuzz_seeds PRIVATE "${PROJECT_SOURCE_DIR}/tests/slsk")
target_link_libraries(slsk_fuzz_seeds PRIVATE selecta_slsk)
selecta_set_warnings(slsk_fuzz_seeds)
