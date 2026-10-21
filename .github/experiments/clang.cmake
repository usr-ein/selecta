# P-028 experiment only (CMAKE_PROJECT_INCLUDE): upstream Clang warns about designated initializers that leave fields to
# their defaults, which Apple Clang doesn't. The flag goes after each target's own warnings, so -Wextra doesn't turn it
# back on. Not for merging.
function(_p028_quiet dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(STATIC_LIBRARY|EXECUTABLE|OBJECT_LIBRARY)$")
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-designated-field-initializers>)
        endif()
    endforeach()
    get_property(subdirectories DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        _p028_quiet("${subdirectory}")
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _p028_quiet "${CMAKE_SOURCE_DIR}")
