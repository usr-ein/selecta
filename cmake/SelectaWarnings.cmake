# SPDX-License-Identifier: GPL-3.0-or-later
# selecta_set_warnings(<target>): warnings (and, with SELECTA_CLANG_TIDY, clang-tidy) for Selecta's own code.
# Third-party code never uses it.

if(SELECTA_CLANG_TIDY)
    find_program(SELECTA_CLANG_TIDY_EXE NAMES clang-tidy
        HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
    message(STATUS "clang-tidy: ${SELECTA_CLANG_TIDY_EXE}")
endif()

function(selecta_set_warnings target)
    if(SELECTA_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${SELECTA_CLANG_TIDY_EXE}")
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(SELECTA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
        -Wnull-dereference -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>)
    if(SELECTA_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
