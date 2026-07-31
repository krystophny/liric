if(NOT DEFINED ARGS_FILE OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ARGS_FILE and WORKDIR are required")
endif()

if(NOT EXISTS "${ARGS_FILE}")
    message(FATAL_ERROR
        "lfortran baseline configure argument record not found: ${ARGS_FILE}")
endif()

file(STRINGS "${ARGS_FILE}" baseline_args)

set(include_dir "")
foreach(arg IN LISTS baseline_args)
    if(arg MATCHES "^-DLIRIC_INCLUDE_DIR=(.+)$")
        set(include_dir "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(include_dir STREQUAL "")
    message(FATAL_ERROR
        "lfortran baseline configure arguments do not define "
        "LIRIC_INCLUDE_DIR; the LLVM baseline cannot find "
        "<liric/liric_session.h>:\n${baseline_args}")
endif()

if(NOT IS_DIRECTORY "${include_dir}")
    message(FATAL_ERROR
        "LIRIC_INCLUDE_DIR is not a directory: ${include_dir}")
endif()

if(NOT EXISTS "${include_dir}/liric/liric_session.h")
    message(FATAL_ERROR
        "LIRIC_INCLUDE_DIR does not provide liric/liric_session.h: "
        "${include_dir}")
endif()

if(NOT DEFINED C_COMPILER OR C_COMPILER STREQUAL "")
    message(STATUS "no C compiler provided; skipping compile probe")
    return()
endif()

set(root "${WORKDIR}/bench_matrix_lfortran_header_contract")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")

set(probe "${root}/probe.c")
file(WRITE "${probe}"
"#include <liric/liric_session.h>\n"
"int main() { return 0; }\n")

# Positive: a fresh build tree compiles the session header using only the
# baseline include contract.
execute_process(
    COMMAND "${C_COMPILER}" -fsyntax-only "-I${include_dir}" "${probe}"
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE ok_rc
    OUTPUT_VARIABLE ok_out
    ERROR_VARIABLE ok_err
)
if(NOT ok_rc EQUAL 0)
    message(FATAL_ERROR
        "baseline include contract failed to compile "
        "<liric/liric_session.h> rc=${ok_rc}\n"
        "stdout:\n${ok_out}\nstderr:\n${ok_err}")
endif()

# Negative: without the configured include directory the same source must not
# compile, proving the probe really depends on the contract.
execute_process(
    COMMAND "${C_COMPILER}" -fsyntax-only "${probe}"
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE bad_rc
    OUTPUT_VARIABLE bad_out
    ERROR_VARIABLE bad_err
)
if(bad_rc EQUAL 0)
    message(FATAL_ERROR
        "<liric/liric_session.h> compiled without LIRIC_INCLUDE_DIR; "
        "the probe does not exercise the include contract\n"
        "stdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()

message(STATUS "lfortran baseline header contract ok: ${include_dir}")
