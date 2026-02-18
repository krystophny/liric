if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_options_hygiene")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"good_case\n")
file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"good_case\",\"options\":\"\",\"source\":\"good_case.f90\"}\n"
"{\"name\":\"stale_case\",\"options\":\"\",\"source\":\"stale_case.f90\"}\n")

file(WRITE "${test_dir}/good_case.f90"
"program good_case\n"
"print *, 42\n"
"end program\n")

find_program(TRUE_BIN NAMES true)
if(NOT TRUE_BIN)
    message(FATAL_ERROR "Unable to locate `true` executable")
endif()

execute_process(
    COMMAND "${BENCH_API}"
        --lfortran "${TRUE_BIN}"
        --lfortran-liric "${TRUE_BIN}"
        --test-dir "${test_dir}"
        --bench-dir "${bench_dir}"
        --timeout 1
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(rc EQUAL 0)
    message(FATAL_ERROR "bench_api should fail preflight on stale compat options rows\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT err MATCHES "compat options preflight failed")
    message(FATAL_ERROR "stderr missing options preflight failure message:\n${err}")
endif()
if(NOT err MATCHES "stale compat options row: stale_case")
    message(FATAL_ERROR "stderr missing stale options detail:\n${err}")
endif()
if(NOT err MATCHES "compat options/list mismatch")
    message(FATAL_ERROR "stderr missing options mismatch terminal error:\n${err}")
endif()

if(EXISTS "${bench_dir}/bench_api.jsonl")
    message(FATAL_ERROR "bench_api.jsonl should not be produced on options preflight failure")
endif()
if(EXISTS "${bench_dir}/bench_api_summary.json")
    message(FATAL_ERROR "bench_api_summary.json should not be produced on options preflight failure")
endif()
if(EXISTS "${bench_dir}/bench_api_failures.jsonl")
    message(FATAL_ERROR "bench_api_failures.jsonl should not be produced on options preflight failure")
endif()
if(EXISTS "${bench_dir}/bench_api_fail_summary.json")
    message(FATAL_ERROR "bench_api_fail_summary.json should not be produced on options preflight failure")
endif()
