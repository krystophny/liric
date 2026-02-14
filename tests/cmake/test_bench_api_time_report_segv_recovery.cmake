if(NOT DEFINED BENCH_API OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_API and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_api_time_report_segv_recovery")
set(bench_dir "${root}/bench")
set(test_dir "${root}/integration_tests")
set(fake_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_liric "${root}/fake_lfortran_liric.sh")
set(invocations "${root}/invocations.log")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${test_dir}")

file(WRITE "${bench_dir}/compat_ll.txt"
"file_03\n"
"file_08\n")

file(WRITE "${bench_dir}/compat_ll_options.jsonl"
"{\"name\":\"file_03\",\"options\":\"\"}\n"
"{\"name\":\"file_08\",\"options\":\"\"}\n")

file(WRITE "${test_dir}/file_03.f90"
"program file_03\n"
"print *, 3\n"
"end program\n")

file(WRITE "${test_dir}/file_08.f90"
"program file_08\n"
"print *, 8\n"
"end program\n")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"src=\"\"\n"
"has_time_report=0\n"
"for arg in \"$@\"; do\n"
"  if [[ \"$arg\" == \"--time-report\" ]]; then\n"
"    has_time_report=1\n"
"  fi\n"
"  src=\"$arg\"\n"
"done\n"
"echo \"llvm has_time_report=$has_time_report src=$src\" >> \"${invocations}\"\n"
"if [[ \"$has_time_report\" -eq 0 ]]; then\n"
"  echo \"program output\"\n"
"  exit 0\n"
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
"has_time_report=0\n"
"for arg in \"$@\"; do\n"
"  if [[ \"$arg\" == \"--time-report\" ]]; then\n"
"    has_time_report=1\n"
"  fi\n"
"  src=\"$arg\"\n"
"done\n"
"echo \"liric has_time_report=$has_time_report src=$src\" >> \"${invocations}\"\n"
"if [[ \"$has_time_report\" -eq 1 && (\"$src\" == *\"file_03.f90\" || \"$src\" == *\"file_08.f90\") ]]; then\n"
"  kill -s SEGV $$\n"
"fi\n"
"if [[ \"$has_time_report\" -eq 0 ]]; then\n"
"  echo \"program output\"\n"
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
        --iters 1
        --timeout 5
        --min-completed 2
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_api should recover from --time-report segv path\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(jsonl "${bench_dir}/bench_api.jsonl")
set(summary "${bench_dir}/bench_api_summary.json")

if(NOT EXISTS "${jsonl}")
    message(FATAL_ERROR "missing bench_api.jsonl")
endif()
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing bench_api_summary.json")
endif()
if(NOT EXISTS "${invocations}")
    message(FATAL_ERROR "missing invocations log")
endif()

file(READ "${jsonl}" jsonl_text)
if(NOT jsonl_text MATCHES "\"name\":\"file_03\".*\"status\":\"ok\".*\"time_report_fallback\":true")
    message(FATAL_ERROR "jsonl missing recovered file_03 row:\n${jsonl_text}")
endif()
if(NOT jsonl_text MATCHES "\"name\":\"file_08\".*\"status\":\"ok\".*\"time_report_fallback\":true")
    message(FATAL_ERROR "jsonl missing recovered file_08 row:\n${jsonl_text}")
endif()
if(jsonl_text MATCHES "\"status\":\"skipped\"")
    message(FATAL_ERROR "jsonl unexpectedly contains skipped row:\n${jsonl_text}")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"attempted\": 2")
    message(FATAL_ERROR "summary missing attempted count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed\": 2")
    message(FATAL_ERROR "summary missing completed count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"skipped\": 0")
    message(FATAL_ERROR "summary missing skipped count:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"completed_time_report_fallback\": 2")
    message(FATAL_ERROR "summary missing time-report fallback count:\n${summary_text}")
endif()

file(READ "${invocations}" invocations_text)
if(NOT invocations_text MATCHES "liric has_time_report=1 src=.*file_03\\.f90")
    message(FATAL_ERROR "invocation log missing initial liric --time-report run for file_03:\n${invocations_text}")
endif()
if(NOT invocations_text MATCHES "liric has_time_report=0 src=.*file_03\\.f90")
    message(FATAL_ERROR "invocation log missing recovered liric no-time-report run for file_03:\n${invocations_text}")
endif()
if(NOT invocations_text MATCHES "liric has_time_report=1 src=.*file_08\\.f90")
    message(FATAL_ERROR "invocation log missing initial liric --time-report run for file_08:\n${invocations_text}")
endif()
if(NOT invocations_text MATCHES "liric has_time_report=0 src=.*file_08\\.f90")
    message(FATAL_ERROR "invocation log missing recovered liric no-time-report run for file_08:\n${invocations_text}")
endif()
