if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_subprocess_overhead_summary")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"overhead_case\n")

file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"overhead_case\",\"options\":\"\"}\n")

file(WRITE "${test_dir}/overhead_case.f90"
"program overhead_case\n"
"print *, 1\n"
"end program\n")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"sleep 0.06\n"
"cat <<'OUT'\n"
"File reading: 4.0\n"
"Src -> ASR: 4.0\n"
"ASR passes (total): 4.0\n"
"ASR -> mod: 4.0\n"
"LLVM IR creation: 4.0\n"
"LLVM opt: 4.0\n"
"LLVM -> JIT: 11.0\n"
"JIT run: 15.0\n"
"Total time: 50.0\n"
"OUT\n"
"exit 0\n")

file(WRITE "${fake_liric}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"sleep 0.06\n"
"cat <<'OUT'\n"
"File reading: 3.0\n"
"Src -> ASR: 3.0\n"
"ASR passes (total): 3.0\n"
"ASR -> mod: 3.0\n"
"LLVM IR creation: 3.0\n"
"LLVM opt: 3.0\n"
"LLVM -> JIT: 10.0\n"
"JIT run: 12.0\n"
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
        --timeout 5
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_api should pass when subprocess overhead summary is emitted\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(summary "${bench_dir}/bench_api_summary.json")
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing bench_api_summary.json")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"measurement_contract_version\": ")
    message(FATAL_ERROR "summary missing measurement_contract_version:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"subprocess_overhead\": \\{")
    message(FATAL_ERROR "summary missing subprocess_overhead object:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"has_data\": true")
    message(FATAL_ERROR "subprocess_overhead.has_data should be true:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"samples\": 1")
    message(FATAL_ERROR "subprocess_overhead.samples should be 1:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"method\": \"elapsed_ms_minus_time_report_total\"")
    message(FATAL_ERROR "summary missing subprocess_overhead method:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"shared_floor_median_ms\": ")
    message(FATAL_ERROR "summary missing shared_floor_median_ms:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"adjusted_wall_speedup_median\": ")
    message(FATAL_ERROR "summary missing adjusted_wall_speedup_median:\n${summary_text}")
endif()
if(summary_text MATCHES "\"observed_wall_speedup_median\": 0\\.000000")
    message(FATAL_ERROR "observed_wall_speedup_median should be non-zero:\n${summary_text}")
endif()
