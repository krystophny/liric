if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_strict")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")
set(fake_probe "${root}/fake_probe_runner.sh")
set(runtime_lib "${root}/libfake_runtime.so")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"good_case\n"
"missing_case\n"
"liric_run_fail\n")
file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"good_case\",\"options\":\"\"}\n"
"{\"name\":\"missing_case\",\"options\":\"\"}\n"
"{\"name\":\"liric_run_fail\",\"options\":\"\"}\n")

file(WRITE "${test_dir}/good_case.f90"
"program good_case\n"
"print *, 42\n"
"end program\n")
file(WRITE "${test_dir}/liric_run_fail.f90"
"program liric_run_fail\n"
"print *, 42\n"
"end program\n")

file(WRITE "${runtime_lib}" "fake")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"out=\"\"\n"
"src=\"\"\n"
"while [[ $# -gt 0 ]]; do\n"
"  if [[ \"$1\" == \"-o\" ]]; then\n"
"    out=\"$2\"\n"
"    shift 2\n"
"  else\n"
"    src=\"$1\"\n"
"    shift\n"
"  fi\n"
"done\n"
"if [[ -z \"$out\" ]]; then\n"
"  exit 1\n"
"fi\n"
"cat > \"$out\" <<'BIN'\n"
"#!/usr/bin/env bash\n"
"echo ok\n"
"exit 0\n"
"BIN\n"
"chmod +x \"$out\"\n"
"exit 0\n")

file(WRITE "${fake_liric}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"out=\"\"\n"
"src=\"\"\n"
"while [[ $# -gt 0 ]]; do\n"
"  if [[ \"$1\" == \"-o\" ]]; then\n"
"    out=\"$2\"\n"
"    shift 2\n"
"  else\n"
"    src=\"$1\"\n"
"    shift\n"
"  fi\n"
"done\n"
"if [[ -z \"$out\" ]]; then\n"
"  exit 1\n"
"fi\n"
"if [[ \"$src\" == *\"liric_run_fail.f90\" ]]; then\n"
"  cat > \"$out\" <<'BIN'\n"
"#!/usr/bin/env bash\n"
"echo fail\n"
"exit 1\n"
"BIN\n"
"else\n"
"  cat > \"$out\" <<'BIN'\n"
"#!/usr/bin/env bash\n"
"echo ok\n"
"exit 0\n"
"BIN\n"
"fi\n"
"chmod +x \"$out\"\n"
"exit 0\n")

file(WRITE "${fake_probe}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"exit 0\n")

execute_process(COMMAND chmod +x "${fake_llvm}" "${fake_liric}" "${fake_probe}"
                RESULT_VARIABLE chmod_rc)
if(NOT chmod_rc EQUAL 0)
    message(FATAL_ERROR "chmod failed")
endif()

execute_process(
    COMMAND "${BENCH_API}"
        --lfortran "${fake_llvm}"
        --lfortran-liric "${fake_liric}"
        --probe-runner "${fake_probe}"
        --runtime-lib "${runtime_lib}"
        --test-dir "${test_dir}"
        --bench-dir "${bench_dir}"
        --iters 1
        --timeout 5
        --min-completed 2
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(rc EQUAL 0)
    message(FATAL_ERROR "bench_api should fail completion gate but passed\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(jsonl "${bench_dir}/bench_api.jsonl")
set(summary "${bench_dir}/bench_api_summary.json")

if(NOT EXISTS "${jsonl}")
    message(FATAL_ERROR "missing bench_api.jsonl")
endif()
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing bench_api_summary.json")
endif()

file(READ "${jsonl}" jsonl_text)
if(NOT jsonl_text MATCHES "\"name\":\"good_case\"")
    message(FATAL_ERROR "jsonl missing good_case row:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"status\":\"ok\"")
    message(FATAL_ERROR "jsonl missing ok status:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"reason\":\"source_missing\"")
    message(FATAL_ERROR "jsonl missing source_missing reason:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"reason\":\"liric_run_failed\"")
    message(FATAL_ERROR "jsonl missing liric_run_failed reason:\n${jsonl_text}")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"attempted\": 3")
    message(FATAL_ERROR "summary missing attempted count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed\": 1")
    message(FATAL_ERROR "summary missing completed count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completion_threshold_met\": false")
    message(FATAL_ERROR "summary missing completion gate status:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"source_missing\": 1")
    message(FATAL_ERROR "summary missing source_missing bucket:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"liric_run_failed\": 1")
    message(FATAL_ERROR "summary missing liric_run_failed bucket:\n${summary_text}")
endif()
