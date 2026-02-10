if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_nonzero_compat")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"ok_case\n"
"nonzero_case\n")

file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"ok_case\",\"options\":\"\"}\n"
"{\"name\":\"nonzero_case\",\"options\":\"\"}\n")

file(WRITE "${test_dir}/ok_case.f90"
"program ok_case\n"
"print *, 42\n"
"end program\n")

file(WRITE "${test_dir}/nonzero_case.f90"
"program nonzero_case\n"
"stop 2\n"
"end program\n")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"src=\"\"\n"
"for arg in \"$@\"; do\n"
"  src=\"$arg\"\n"
"done\n"
"if [[ \"$src\" == *\"nonzero_case.f90\" ]]; then\n"
"  echo runtime-stop\n"
"  echo error: runtime-stop >&2\n"
"  exit 2\n"
"fi\n"
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
"if [[ \"$src\" == *\"nonzero_case.f90\" ]]; then\n"
"  echo runtime-stop\n"
"  echo error: runtime-stop >&2\n"
"  exit 2\n"
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
        --iters 1
        --timeout 5
        --min-completed 2
        --lookup-dispatch-share-pct 0.2
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_api should pass with compatible non-zero outcomes\nstdout:\n${out}\nstderr:\n${err}")
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
if(NOT jsonl_text MATCHES "\"name\":\"ok_case\".*\"status\":\"ok\"")
    message(FATAL_ERROR "jsonl missing ok_case success row:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"nonzero_case\".*\"status\":\"ok_nonzero_compat\".*\"rc\":2")
    message(FATAL_ERROR "jsonl missing nonzero compatibility row:\n${jsonl_text}")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"attempted\": 2")
    message(FATAL_ERROR "summary missing attempted count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed\": 2")
    message(FATAL_ERROR "summary missing completed count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed_timed\": 1")
    message(FATAL_ERROR "summary missing completed_timed count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed_nonzero_compat\": 1")
    message(FATAL_ERROR "summary missing completed_nonzero_compat count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"skipped\": 0")
    message(FATAL_ERROR "summary missing skipped count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completion_threshold_met\": true")
    message(FATAL_ERROR "summary missing completion gate status:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"phase_tracker\": \\{")
    message(FATAL_ERROR "summary missing phase_tracker object:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"liric_llvm_ir_avg_median_ms\": 5.500000")
    message(FATAL_ERROR "summary missing liric llvm ir metric:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"lookup_dispatch_share_pct\": 0.200000")
    message(FATAL_ERROR "summary missing lookup_dispatch_share_pct metric:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"lookup_dispatch_met\": true")
    message(FATAL_ERROR "summary missing lookup_dispatch_met criterion:\n${summary_text}")
endif()
