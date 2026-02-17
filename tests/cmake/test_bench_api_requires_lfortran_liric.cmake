if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_requires_lfortran_liric")
set(bench_dir "${root}/bench")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")

find_program(TRUE_EXE true)
if(NOT TRUE_EXE)
    message(FATAL_ERROR "true executable is required")
endif()

execute_process(
    COMMAND "${BENCH_API}"
        --bench-dir "${bench_dir}"
        --lfortran "${TRUE_EXE}"
        --allow-empty
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(rc EQUAL 0)
    message(FATAL_ERROR
        "bench_api should fail without a WITH_LIRIC binary\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

if(NOT err MATCHES "lfortran \\(WITH_LIRIC\\) not found")
    message(FATAL_ERROR
        "missing expected WITH_LIRIC preflight error\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

if(EXISTS "${bench_dir}/bench_api_summary.json")
    message(FATAL_ERROR "bench_api_summary.json must not be produced on binary preflight failure")
endif()
