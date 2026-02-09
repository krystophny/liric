if(NOT DEFINED RUNNER)
    message(FATAL_ERROR "RUNNER variable is required")
endif()

if(NOT EXISTS "${RUNNER}")
    message(FATAL_ERROR "runner file not found: ${RUNNER}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping nightly_mass shell runner test")
    return()
endif()

find_program(JQ_EXE jq)
if(NOT JQ_EXE)
    message(STATUS "jq not available; skipping nightly_mass shell runner test")
    return()
endif()

if(NOT DEFINED WORKDIR)
    set(WORKDIR "${CMAKE_BINARY_DIR}")
endif()

set(TEST_ROOT "${WORKDIR}/nightly_mass_shell_runner")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

set(COMPAT_PASS "${TEST_ROOT}/compat_pass.jsonl")
set(COMPAT_FAIL "${TEST_ROOT}/compat_fail.jsonl")
set(BASELINE_FAIL "${TEST_ROOT}/baseline_fail.jsonl")

file(WRITE "${COMPAT_PASS}"
    "{\"name\":\"case_ok_1\",\"source\":\"/tmp/a.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":true,\"lli_ok\":true,\"liric_match\":true,\"lli_match\":true,\"llvm_rc\":0,\"liric_rc\":0,\"lli_rc\":0,\"error\":\"\"}\n"
    "{\"name\":\"case_ok_2\",\"source\":\"/tmp/b.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":true,\"lli_ok\":true,\"liric_match\":true,\"lli_match\":true,\"llvm_rc\":0,\"liric_rc\":0,\"lli_rc\":0,\"error\":\"\"}\n"
)

set(OUT_PASS "${TEST_ROOT}/out_pass")
execute_process(
    COMMAND "${BASH_EXE}" "${RUNNER}"
        --compat-jsonl "${COMPAT_PASS}"
        --output-root "${OUT_PASS}"
    RESULT_VARIABLE pass_rc
    OUTPUT_VARIABLE pass_out
    ERROR_VARIABLE pass_err
)
if(NOT pass_rc EQUAL 0)
    message(FATAL_ERROR
        "nightly_mass pass run failed\nstdout:\n${pass_out}\nstderr:\n${pass_err}"
    )
endif()

foreach(path
    "${OUT_PASS}/manifest_tests_toml.jsonl"
    "${OUT_PASS}/selection_decisions.jsonl"
    "${OUT_PASS}/results.jsonl"
    "${OUT_PASS}/summary.json"
    "${OUT_PASS}/summary.md"
    "${OUT_PASS}/failures.csv"
)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "expected artifact missing: ${path}")
    endif()
endforeach()

file(READ "${OUT_PASS}/summary.json" summary_pass)
if(NOT summary_pass MATCHES "\"manifest_total\"[ \t]*:[ \t]*2")
    message(FATAL_ERROR "expected manifest_total=2 in pass summary")
endif()
if(NOT summary_pass MATCHES "\"mismatch_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected mismatch_count=0 in pass summary")
endif()
if(NOT summary_pass MATCHES "\"gate_fail\"[ \t]*:[ \t]*false")
    message(FATAL_ERROR "expected gate_fail=false in pass summary")
endif()

file(WRITE "${COMPAT_FAIL}"
    "{\"name\":\"case_mismatch\",\"source\":\"/tmp/c.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":true,\"lli_ok\":true,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":0,\"liric_rc\":0,\"lli_rc\":0,\"error\":\"\"}\n"
)
file(WRITE "${BASELINE_FAIL}"
    "{\"case_id\":\"case_mismatch\",\"classification\":\"pass\"}\n"
)

set(OUT_FAIL "${TEST_ROOT}/out_fail")
execute_process(
    COMMAND "${BASH_EXE}" "${RUNNER}"
        --compat-jsonl "${COMPAT_FAIL}"
        --output-root "${OUT_FAIL}"
        --baseline "${BASELINE_FAIL}"
    RESULT_VARIABLE fail_rc
    OUTPUT_VARIABLE fail_out
    ERROR_VARIABLE fail_err
)
if(fail_rc EQUAL 0)
    message(FATAL_ERROR
        "nightly_mass fail run unexpectedly passed\nstdout:\n${fail_out}\nstderr:\n${fail_err}"
    )
endif()

file(READ "${OUT_FAIL}/summary.json" summary_fail)
if(NOT summary_fail MATCHES "\"mismatch_count\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected mismatch_count=1 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"new_supported_regressions\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected new_supported_regressions=1 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"gate_fail\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "expected gate_fail=true in fail summary")
endif()
