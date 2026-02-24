if(NOT DEFINED LFORTRAN_ROOT OR "${LFORTRAN_ROOT}" STREQUAL "")
    message(FATAL_ERROR "LFORTRAN_ROOT is required")
endif()
if(NOT DEFINED LFORTRAN_REPO OR "${LFORTRAN_REPO}" STREQUAL "")
    message(FATAL_ERROR "LFORTRAN_REPO is required")
endif()
if(NOT DEFINED LFORTRAN_REF OR "${LFORTRAN_REF}" STREQUAL "")
    message(FATAL_ERROR "LFORTRAN_REF is required")
endif()
if(NOT DEFINED LFORTRAN_REMOTE OR "${LFORTRAN_REMOTE}" STREQUAL "")
    set(LFORTRAN_REMOTE "origin")
endif()

find_program(GIT_EXE git)
if(NOT GIT_EXE)
    message(FATAL_ERROR "git is required")
endif()

get_filename_component(_lfortran_parent "${LFORTRAN_ROOT}" DIRECTORY)
file(MAKE_DIRECTORY "${_lfortran_parent}")

if(NOT EXISTS "${LFORTRAN_ROOT}/.git")
    execute_process(
        COMMAND "${GIT_EXE}" clone "${LFORTRAN_REPO}" "${LFORTRAN_ROOT}"
        RESULT_VARIABLE clone_rc
    )
    if(NOT clone_rc EQUAL 0)
        message(FATAL_ERROR "failed to clone lfortran repo: ${LFORTRAN_REPO}")
    endif()
endif()

if(LFORTRAN_REF MATCHES "^${LFORTRAN_REMOTE}/")
    execute_process(
        COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" remote get-url "${LFORTRAN_REMOTE}"
        OUTPUT_VARIABLE remote_url
        RESULT_VARIABLE remote_get_url_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(remote_get_url_rc EQUAL 0)
        if(NOT "${remote_url}" STREQUAL "${LFORTRAN_REPO}")
            execute_process(
                COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" remote set-url "${LFORTRAN_REMOTE}" "${LFORTRAN_REPO}"
                RESULT_VARIABLE remote_set_url_rc
            )
            if(NOT remote_set_url_rc EQUAL 0)
                message(FATAL_ERROR "failed to update lfortran remote URL: ${LFORTRAN_REMOTE}")
            endif()
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" remote add "${LFORTRAN_REMOTE}" "${LFORTRAN_REPO}"
            RESULT_VARIABLE remote_add_rc
        )
        if(NOT remote_add_rc EQUAL 0)
            message(FATAL_ERROR "failed to add lfortran remote: ${LFORTRAN_REMOTE}")
        endif()
    endif()
endif()

execute_process(
    COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" fetch --all --tags
    RESULT_VARIABLE fetch_rc
)
if(NOT fetch_rc EQUAL 0)
    message(FATAL_ERROR "failed to fetch lfortran refs")
endif()

execute_process(
    COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" checkout "${LFORTRAN_REF}"
    RESULT_VARIABLE checkout_rc
)
if(NOT checkout_rc EQUAL 0)
    message(FATAL_ERROR "failed to checkout lfortran ref: ${LFORTRAN_REF}")
endif()

execute_process(
    COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" reset --hard "${LFORTRAN_REF}"
    RESULT_VARIABLE reset_rc
)
if(NOT reset_rc EQUAL 0)
    message(FATAL_ERROR "failed to reset lfortran checkout to ref: ${LFORTRAN_REF}")
endif()

set(_lfortran_version_file "${LFORTRAN_ROOT}/version")
if(NOT EXISTS "${_lfortran_version_file}")
    execute_process(
        COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" describe --tags --always --dirty
        RESULT_VARIABLE describe_rc
        OUTPUT_VARIABLE lfortran_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT describe_rc EQUAL 0 OR "${lfortran_version}" STREQUAL "")
        set(lfortran_version "0.0.0-unknown")
    else()
        string(REGEX REPLACE "^v" "" lfortran_version "${lfortran_version}")
    endif()
    file(WRITE "${_lfortran_version_file}" "${lfortran_version}\n")
    message(STATUS "Generated missing LFortran version file: ${lfortran_version}")
endif()

execute_process(
    COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" rev-parse HEAD
    RESULT_VARIABLE head_rc
    OUTPUT_VARIABLE _lfortran_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT head_rc EQUAL 0 OR "${_lfortran_head}" STREQUAL "")
    message(FATAL_ERROR "failed to read lfortran HEAD commit")
endif()

set(_lfortran_generated_preprocessor
    "${LFORTRAN_ROOT}/src/lfortran/parser/preprocessor.cpp")
set(_lfortran_generated_parser_cc
    "${LFORTRAN_ROOT}/src/lfortran/parser/parser.tab.cc")
set(_lfortran_generated_parser_hh
    "${LFORTRAN_ROOT}/src/lfortran/parser/parser.tab.hh")
set(_lfortran_prepare_head_file
    "${LFORTRAN_ROOT}/.liric_prepare_head")
set(_lfortran_need_generated_refresh OFF)

if(NOT EXISTS "${_lfortran_generated_preprocessor}" OR
   NOT EXISTS "${_lfortran_generated_parser_cc}" OR
   NOT EXISTS "${_lfortran_generated_parser_hh}")
    set(_lfortran_need_generated_refresh ON)
endif()

if(EXISTS "${_lfortran_prepare_head_file}")
    file(READ "${_lfortran_prepare_head_file}" _lfortran_prev_head)
    string(STRIP "${_lfortran_prev_head}" _lfortran_prev_head)
    if(NOT "${_lfortran_prev_head}" STREQUAL "${_lfortran_head}")
        set(_lfortran_need_generated_refresh ON)
    endif()
else()
    set(_lfortran_need_generated_refresh ON)
endif()

if(_lfortran_need_generated_refresh)
    find_program(BASH_EXE bash)
    find_program(RE2C_EXE re2c)
    find_program(BISON_EXE bison)
    if(NOT BASH_EXE OR NOT RE2C_EXE OR NOT BISON_EXE)
        message(FATAL_ERROR
            "missing required tools for LFortran generated sources: bash/re2c/bison")
    endif()

    execute_process(
        COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" tag --list
        RESULT_VARIABLE tag_list_rc
        OUTPUT_VARIABLE tag_list
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT tag_list_rc EQUAL 0)
        message(FATAL_ERROR "failed to list lfortran tags")
    endif()

    if("${tag_list}" STREQUAL "")
        set(_fallback_tag "v0.0.0-liric-aot")
        execute_process(
            COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" rev-parse "${_fallback_tag}"
            RESULT_VARIABLE fallback_tag_exists_rc
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(NOT fallback_tag_exists_rc EQUAL 0)
            execute_process(
                COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" tag "${_fallback_tag}" HEAD
                RESULT_VARIABLE fallback_tag_create_rc
            )
            if(NOT fallback_tag_create_rc EQUAL 0)
                message(FATAL_ERROR "failed to create fallback lfortran tag")
            endif()
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            RE2C=${RE2C_EXE}
            BISON=${BISON_EXE}
            "${BASH_EXE}" -e build0.sh
        WORKING_DIRECTORY "${LFORTRAN_ROOT}"
        RESULT_VARIABLE build0_rc
    )
    if(NOT build0_rc EQUAL 0)
        message(FATAL_ERROR "failed to generate lfortran parser sources with build0.sh")
    endif()
endif()

file(WRITE "${_lfortran_prepare_head_file}" "${_lfortran_head}\n")
