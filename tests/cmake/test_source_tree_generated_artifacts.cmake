if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

if(NOT DEFINED MODE)
    set(MODE "verify")
endif()

get_filename_component(ROOT "${ROOT}" ABSOLUTE)

set(_patterns
    "*.mod"
    "*.MOD"
    "*.smod"
    "*.SMOD"
    "*.tmp.mod"
    "*.tmp.smod"
    "*.bin"
    "_lfortran_generated_file_*"
    "file_*_data.txt"
    "file_common_block_*.mod"
)

set(_leaks)
foreach(_pat IN LISTS _patterns)
    file(GLOB_RECURSE _matches
        RELATIVE "${ROOT}"
        "${ROOT}/${_pat}")
    foreach(_rel IN LISTS _matches)
        if(_rel MATCHES "^\\.git/")
            continue()
        endif()
        if(_rel MATCHES "^build/")
            continue()
        endif()
        if(_rel MATCHES "^build-[^/]+/")
            continue()
        endif()
        if(_rel MATCHES "^Testing/")
            continue()
        endif()
        list(APPEND _leaks "${_rel}")
    endforeach()
endforeach()

list(REMOVE_DUPLICATES _leaks)
list(SORT _leaks)

if(MODE STREQUAL "purge")
    foreach(_rel IN LISTS _leaks)
        file(REMOVE "${ROOT}/${_rel}")
    endforeach()
    message(STATUS "source-tree purge removed ${_leaks}")
    return()
endif()

if(NOT MODE STREQUAL "verify")
    message(FATAL_ERROR "MODE must be purge or verify")
endif()

if(_leaks)
    foreach(_rel IN LISTS _leaks)
        file(REMOVE "${ROOT}/${_rel}")
    endforeach()
    string(JOIN "\n  " _leak_lines ${_leaks})
    message(FATAL_ERROR
        "generated artifacts leaked into source tree and were removed:\n  ${_leak_lines}")
endif()
