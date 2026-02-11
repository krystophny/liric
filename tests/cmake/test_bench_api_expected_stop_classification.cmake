if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_expected_stop")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"ok_case\n"
"stop_case\n")

file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"ok_case\",\"options\":\"\",\"source\":\"ok_case.f90\"}\n"
"{\"name\":\"stop_case\",\"options\":\"\",\"source\":\"stop_case.f90\"}\n")

file(WRITE "${test_dir}/ok_case.f90"
"program ok_case\n"
"print *, 42\n"
"end program\n")

file(WRITE "${test_dir}/stop_case.f90"
"program stop_case\n"
"stop 3\n"
"end program\n")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"src=\"\"\n"
"for arg in \"$@\"; do\n"
"  src=\"$arg\"\n"
"done\n"
"if [[ \"$src\" == *\"stop_case.f90\" ]]; then\n"
"echo \"STOP 3\" >&2\n"
"exit 3\n"
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
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_api should classify expected stop as skip without hard failure\nstdout:\n${out}\nstderr:\n${err}")
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
if(NOT jsonl_text MATCHES "\"name\":\"stop_case\".*\"status\":\"skipped\".*\"reason\":\"llvm_jit_expected_nonzero_or_stop\"")
    message(FATAL_ERROR "jsonl missing expected stop skip classification:\n${jsonl_text}")
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
if(NOT summary_text MATCHES "\"llvm_jit_expected_nonzero_or_stop\": 1")
    message(FATAL_ERROR "summary missing llvm_jit_expected_nonzero_or_stop bucket:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"llvm_jit_failed\": 0")
    message(FATAL_ERROR "summary missing llvm_jit_failed zero bucket:\n${summary_text}")
endif()
