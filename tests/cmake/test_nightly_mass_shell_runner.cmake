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
set(COMPAT_EMIT_FAIL "${TEST_ROOT}/compat_emit_fail.jsonl")
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
    "${OUT_PASS}/unsupported_bucket_coverage.md"
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
if(NOT summary_pass MATCHES "\"lfortran_emit_fail_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected lfortran_emit_fail_count=0 in pass summary")
endif()
if(NOT summary_pass MATCHES "\"liric_compat_failure_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected liric_compat_failure_count=0 in pass summary")
endif()
if(NOT summary_pass MATCHES "\"gate_fail_reasons\"[ \t]*:[ \t]*\\[[ \t\n\r]*\\]")
    message(FATAL_ERROR "expected empty gate_fail_reasons in pass summary")
endif()
if(NOT summary_pass MATCHES "\"gate_fail\"[ \t]*:[ \t]*false")
    message(FATAL_ERROR "expected gate_fail=false in pass summary")
endif()

file(WRITE "${COMPAT_FAIL}"
    "{\"name\":\"case_mismatch\",\"source\":\"/tmp/c.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":true,\"lli_ok\":true,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":0,\"liric_rc\":0,\"lli_rc\":0,\"error\":\"\"}\n"
    "{\"name\":\"case_rc_mismatch\",\"source\":\"/tmp/e.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":true,\"lli_ok\":true,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":0,\"liric_rc\":1,\"lli_rc\":1,\"error\":\"\"}\n"
    "{\"name\":\"case_unresolved_symbol\",\"source\":\"/tmp/d.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":false,\"lli_ok\":false,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":0,\"liric_rc\":-1,\"lli_rc\":-1,\"error\":\"unresolved symbol: _lfortran_printf\"}\n"
    "{\"name\":\"case_runtime_failure\",\"source\":\"/tmp/f.f90\",\"options\":\"\",\"llvm_ok\":true,\"liric_ok\":false,\"lli_ok\":false,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":0,\"liric_rc\":-11,\"lli_rc\":-11,\"error\":\"segmentation fault\"}\n"
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
if(NOT summary_fail MATCHES "\"mismatch_count\"[ \t]*:[ \t]*2")
    message(FATAL_ERROR "expected mismatch_count=2 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"lfortran_emit_fail_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected lfortran_emit_fail_count=0 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"liric_compat_failure_count\"[ \t]*:[ \t]*4")
    message(FATAL_ERROR "expected liric_compat_failure_count=4 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"new_supported_regressions\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected new_supported_regressions=1 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"gate_fail\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "expected gate_fail=true in fail summary")
endif()
if(NOT summary_fail MATCHES "\"gate_fail_reasons\"[ \t]*:[ \t]*\\[[^]]*\"mismatch\"")
    message(FATAL_ERROR "expected mismatch gate reason in fail summary")
endif()
if(NOT summary_fail MATCHES "\"gate_fail_reasons\"[ \t]*:[ \t]*\\[[^]]*\"new_supported_regressions\"")
    message(FATAL_ERROR "expected new_supported_regressions gate reason in fail summary")
endif()
if(NOT summary_fail MATCHES "\"unsupported_abi\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected unsupported_abi=1 in fail summary")
endif()
if(NOT summary_fail MATCHES "\"unsupported_feature\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected unsupported_feature=1 in fail summary")
endif()
if(NOT summary_fail MATCHES "output-format\\|rc-mismatch\\|general")
    message(FATAL_ERROR "expected rc-mismatch taxonomy bucket in fail summary")
endif()
if(NOT summary_fail MATCHES "jit-link\\|unresolved-symbol\\|runtime-api")
    message(FATAL_ERROR "expected unresolved-symbol taxonomy bucket in fail summary")
endif()
if(NOT summary_fail MATCHES "runtime\\|unsupported-feature\\|general")
    message(FATAL_ERROR "expected runtime unsupported-feature taxonomy bucket in fail summary")
endif()
if(NOT summary_fail MATCHES "\"mapped\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "expected unsupported bucket coverage entry to be mapped")
endif()
if(NOT summary_fail MATCHES "\"#[0-9][0-9]*\"")
    message(FATAL_ERROR "expected unsupported bucket issue mapping to include an issue reference")
endif()

file(READ "${OUT_FAIL}/summary.md" summary_fail_md)
if(NOT summary_fail_md MATCHES "## Taxonomy Counts \\(Mismatch\\)")
    message(FATAL_ERROR "expected mismatch taxonomy section in fail summary markdown")
endif()
if(NOT summary_fail_md MATCHES "output-format\\|rc-mismatch\\|general")
    message(FATAL_ERROR "expected rc-mismatch entry in fail summary markdown")
endif()
if(NOT summary_fail_md MATCHES "runtime\\|unsupported-feature\\|general")
    message(FATAL_ERROR "expected runtime unsupported-feature entry in fail summary markdown")
endif()

file(WRITE "${COMPAT_EMIT_FAIL}"
    "{\"name\":\"case_emit_fail\",\"source\":\"/tmp/g.f90\",\"options\":\"\",\"llvm_ok\":false,\"liric_ok\":false,\"lli_ok\":false,\"liric_match\":false,\"lli_match\":false,\"llvm_rc\":1,\"liric_rc\":1,\"lli_rc\":1,\"error\":\"lfortran codegen failure\"}\n"
)

set(OUT_EMIT_FAIL "${TEST_ROOT}/out_emit_fail")
execute_process(
    COMMAND "${BASH_EXE}" "${RUNNER}"
        --compat-jsonl "${COMPAT_EMIT_FAIL}"
        --output-root "${OUT_EMIT_FAIL}"
    RESULT_VARIABLE emit_fail_rc
    OUTPUT_VARIABLE emit_fail_out
    ERROR_VARIABLE emit_fail_err
)
if(emit_fail_rc EQUAL 0)
    message(FATAL_ERROR
        "nightly_mass emit-fail run unexpectedly passed\nstdout:\n${emit_fail_out}\nstderr:\n${emit_fail_err}"
    )
endif()

file(READ "${OUT_EMIT_FAIL}/summary.json" summary_emit_fail)
if(NOT summary_emit_fail MATCHES "\"mismatch_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected mismatch_count=0 in emit-fail summary")
endif()
if(NOT summary_emit_fail MATCHES "\"lfortran_emit_fail_count\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "expected lfortran_emit_fail_count=1 in emit-fail summary")
endif()
if(NOT summary_emit_fail MATCHES "\"liric_compat_failure_count\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "expected liric_compat_failure_count=0 in emit-fail summary")
endif()
if(NOT summary_emit_fail MATCHES "\"gate_fail_reasons\"[ \t]*:[ \t]*\\[[^]]*\"lfortran_emit_fail\"")
    message(FATAL_ERROR "expected lfortran_emit_fail gate reason in emit-fail summary")
endif()
if(NOT summary_emit_fail MATCHES "\"gate_fail\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "expected gate_fail=true in emit-fail summary")
endif()
