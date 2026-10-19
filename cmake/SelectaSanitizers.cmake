# SPDX-License-Identifier: GPL-3.0-or-later
# SELECTA_SANITIZE="address;undefined" or "thread" instruments every target, third-party code included.

if(SELECTA_SANITIZE)
    if(MSVC)
        message(WARNING "SELECTA_SANITIZE is ignored with MSVC")
    else()
        string(REPLACE ";" "," _selecta_sanitizers "${SELECTA_SANITIZE}")
        add_compile_options(-fsanitize=${_selecta_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=${_selecta_sanitizers})
        message(STATUS "Sanitizers: ${_selecta_sanitizers}")
    endif()
endif()
