if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_timeout_diagnostics")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"ok_case\n"
"timeout_case\n")

file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"ok_case\",\"options\":\"\"}\n"
"{\"name\":\"timeout_case\",\"options\":\"\"}\n")

file(WRITE "${test_dir}/ok_case.f90"
"program ok_case\n"
"print *, 42\n"
"end program\n")

file(WRITE "${test_dir}/timeout_case.f90"
"program timeout_case\n"
"print *, 7\n"
"end program\n")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"cat <<'OUT'\n"
"File reading: 1.0\n"
"Src -> ASR: 2.0\n"
"ASR passes (total): 3.0\n"
"ASR -> mod: 4.0\n"
"LLVM IR creation: 5.0\n"
"LLVM opt: 6.0\n"
"LLVM -> JIT: 7.0\n"
"JIT run: 8.0\n"
"Total time: 36.0\n"
"OUT\n"
"exit 0\n")

file(WRITE "${fake_liric}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"src=\"\"\n"
"for arg in \"$@\"; do\n"
"  src=\"$arg\"\n"
"done\n"
"if [[ \"$src\" == *\"timeout_case.f90\" ]]; then\n"
"  echo \"File reading: 1.0\"\n"
"  echo \"Src -> ASR: 2.0\"\n"
"  echo \"heartbeat: parsing module\" >&2\n"
"  sleep 2\n"
"  exit 0\n"
"fi\n"
"cat <<'OUT'\n"
"File reading: 1.5\n"
"Src -> ASR: 2.5\n"
"ASR passes (total): 3.5\n"
"ASR -> mod: 4.5\n"
"LLVM IR creation: 5.5\n"
"LLVM opt: 6.5\n"
"LLVM -> JIT: 7.5\n"
"JIT run: 8.5\n"
"Total time: 40.0\n"
"OUT\n"
"exit 0\n")

execute_process(COMMAND chmod +x "${fake_llvm}" "${fake_liric}"
                RESULT_VARIABLE chmod_rc)
if(NOT chmod_rc EQUAL 0)
    message(FATAL_ERROR "chmod failed")
endif()

execute_process(
    COMMAND "${BENCH_API}"
        --lfortran "${fake_llvm}"
        --lfortran-liric "${fake_liric}"
        --test-dir "${test_dir}"
        --bench-dir "${bench_dir}"
        --timeout-ms 200
        --min-completed 1
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_api should pass with one timed-out skip carrying diagnostics\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(jsonl "${bench_dir}/bench_api.jsonl")
set(fail_jsonl "${bench_dir}/bench_api_failures.jsonl")
set(summary "${bench_dir}/bench_api_summary.json")

if(NOT EXISTS "${jsonl}")
    message(FATAL_ERROR "missing bench_api.jsonl")
endif()
if(NOT EXISTS "${fail_jsonl}")
    message(FATAL_ERROR "missing bench_api_failures.jsonl")
endif()
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing bench_api_summary.json")
endif()

file(READ "${jsonl}" jsonl_text)
if(NOT jsonl_text MATCHES "\"name\":\"ok_case\".*\"status\":\"ok\"")
    message(FATAL_ERROR "jsonl missing ok_case success row:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"status\":\"skipped\".*\"reason\":\"liric_jit_timeout\".*\"timed_out\":true")
    message(FATAL_ERROR "jsonl missing timeout classification row:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"timeout_ms\":200")
    message(FATAL_ERROR "jsonl missing timeout_ms diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"timeout_silent\":false")
    message(FATAL_ERROR "jsonl missing timeout_silent diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"time_report_phase_count\":2")
    message(FATAL_ERROR "jsonl missing time_report_phase_count diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"time_report_last_phase\":\"Src -> ASR\"")
    message(FATAL_ERROR "jsonl missing time_report_last_phase diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"time_report_last_phase_ms\":2.000000")
    message(FATAL_ERROR "jsonl missing time_report_last_phase_ms diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"last_stdout_line\":\"Src -> ASR: 2.0\"")
    message(FATAL_ERROR "jsonl missing last_stdout_line diagnostics:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"timeout_case\".*\"last_stderr_line\":\"heartbeat: parsing module\"")
    message(FATAL_ERROR "jsonl missing last_stderr_line diagnostics:\n${jsonl_text}")
endif()

file(READ "${fail_jsonl}" fail_jsonl_text)
if(NOT fail_jsonl_text MATCHES "\"name\":\"timeout_case\".*\"reason\":\"liric_jit_timeout\".*\"timed_out\":true")
    message(FATAL_ERROR "failure jsonl missing timeout diagnostic row:\n${fail_jsonl_text}")
endif()
if(NOT fail_jsonl_text MATCHES "\"name\":\"timeout_case\".*\"time_report_phase_count\":2")
    message(FATAL_ERROR "failure jsonl missing phase-count diagnostics:\n${fail_jsonl_text}")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"attempted\": 2")
    message(FATAL_ERROR "summary missing attempted count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed\": 1")
    message(FATAL_ERROR "summary missing completed count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"skipped\": 1")
    message(FATAL_ERROR "summary missing skipped count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"liric_jit_timeout\": 1")
    message(FATAL_ERROR "summary missing liric_jit_timeout bucket:\n${summary_text}")
endif()
