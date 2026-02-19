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
        COMMAND "${GIT_EXE}" -C "${LFORTRAN_ROOT}" remote
        OUTPUT_VARIABLE remotes
        RESULT_VARIABLE remote_list_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT remote_list_rc EQUAL 0)
        message(FATAL_ERROR "failed to list lfortran remotes")
    endif()

    if(NOT remotes MATCHES "(^|\\n)${LFORTRAN_REMOTE}(\\n|$)")
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
