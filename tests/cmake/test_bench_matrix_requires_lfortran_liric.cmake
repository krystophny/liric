if(NOT DEFINED BENCH_MATRIX OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_MATRIX and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_matrix_requires_lfortran_liric")
set(bench_dir "${root}/bench")
set(manifest "${CMAKE_CURRENT_LIST_DIR}/../../tools/bench_manifest.json")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")

find_program(TRUE_EXE true)
if(NOT TRUE_EXE)
    message(FATAL_ERROR "true executable is required")
endif()

execute_process(
    COMMAND "${BENCH_MATRIX}"
        --bench-dir "${bench_dir}"
        --manifest "${manifest}"
        --modes isel
        --policies direct
        --lanes api_full_llvm
        --timeout 5
        --timeout-ms 500
        --skip-lfortran-rebuild
        --lfortran "${TRUE_EXE}"
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(rc EQUAL 0)
    message(FATAL_ERROR
        "bench_matrix should fail when WITH_LIRIC lfortran binary is missing\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

set(fails "${bench_dir}/matrix_failures.jsonl")
if(NOT EXISTS "${fails}")
    message(FATAL_ERROR "missing matrix_failures.jsonl:\nstdout:\n${out}\nstderr:\n${err}")
endif()

file(READ "${fails}" fails_text)
if(NOT fails_text MATCHES "\"reason\":\"lfortran_liric_binary_missing\"")
    message(FATAL_ERROR
        "missing lfortran_liric_binary_missing failure reason\nfails:\n${fails_text}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()
