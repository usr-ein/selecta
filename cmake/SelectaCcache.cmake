# SPDX-License-Identifier: GPL-3.0-or-later
# Compiles go through ccache when it's installed, with one cache shared by the main checkout and every worktree in it
# (the crew's minions, docs/CREW.md). Paths are hashed relative to the main checkout, which contains every worktree, so
# the same file compiles to the same cache entry wherever it's checked out. The build folder isn't hashed either
# (CCACHE_NOHASHDIR): a hit made in another worktree carries that worktree's path in its debug info, which only
# matters when stepping into the code in a debugger. A launcher given on the command line (CI) is left alone.
option(SELECTA_CCACHE "Compile through ccache when it is installed" ON)
if(NOT SELECTA_CCACHE OR CMAKE_CXX_COMPILER_LAUNCHER OR WIN32)
    return()
endif()
find_program(SELECTA_CCACHE_PROGRAM ccache)
if(NOT SELECTA_CCACHE_PROGRAM)
    return()
endif()

# The main checkout is the parent of the git directory every worktree shares.
set(_selecta_ccache_base "${PROJECT_SOURCE_DIR}")
execute_process(
    COMMAND git rev-parse --path-format=absolute --git-common-dir
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _selecta_git_common
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _selecta_git_result)
if(_selecta_git_result EQUAL 0 AND _selecta_git_common MATCHES "/\\.git$")
    get_filename_component(_selecta_ccache_base "${_selecta_git_common}" DIRECTORY)
endif()

set(_selecta_launcher /usr/bin/env "CCACHE_BASEDIR=${_selecta_ccache_base}" CCACHE_NOHASHDIR=true
    "${SELECTA_CCACHE_PROGRAM}")
set(CMAKE_C_COMPILER_LAUNCHER ${_selecta_launcher})
set(CMAKE_CXX_COMPILER_LAUNCHER ${_selecta_launcher})
set(CMAKE_OBJCXX_COMPILER_LAUNCHER ${_selecta_launcher})
message(STATUS "ccache: ${SELECTA_CCACHE_PROGRAM}, shared with every worktree of ${_selecta_ccache_base}")
