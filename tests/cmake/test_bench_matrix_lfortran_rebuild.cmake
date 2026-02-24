if(NOT DEFINED BENCH_MATRIX OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_MATRIX and WORKDIR are required")
endif()

if(NOT EXISTS "${BENCH_MATRIX}")
    message(FATAL_ERROR "bench_matrix executable not found: ${BENCH_MATRIX}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping bench_matrix lfortran rebuild test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for bench_matrix lfortran rebuild test")
endif()

set(root "${WORKDIR}/bench_matrix_lfortran_rebuild")
set(bench_dir "${root}/bench")
set(log_dir "${root}/logs")
set(cmake_log "${log_dir}/cmake_args.log")
set(mode_log "${log_dir}/mode.log")
set(fake_cmake "${root}/fake_cmake.sh")
set(fake_compat "${root}/fake_bench_compat_check.sh")
set(fake_api "${root}/fake_bench_api.sh")
set(fake_lfortran_llvm "${root}/fake_lfortran_llvm.sh")
set(fake_lfortran_liric "${root}/fake_lfortran_liric.sh")
set(llvm_build_dir "${root}/lfortran_build_llvm")
set(liric_build_dir "${root}/lfortran_build_liric")
set(manifest "${CMAKE_CURRENT_LIST_DIR}/../../tools/bench_manifest.json")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${log_dir}")
file(MAKE_DIRECTORY "${llvm_build_dir}")
file(MAKE_DIRECTORY "${liric_build_dir}")

file(WRITE "${fake_cmake}" "#!/usr/bin/env bash
set -euo pipefail
echo \"$*\" >> \"${cmake_log}\"
[[ \"$1\" == \"--build\" ]] || { echo \"missing --build\" >&2; exit 90; }
[[ \"$3\" == \"-j\" ]] || { echo \"missing -j\" >&2; exit 91; }
[[ -n \"$2\" ]] || { echo \"missing build dir\" >&2; exit 92; }
[[ -n \"$4\" ]] || { echo \"missing jobs\" >&2; exit 93; }
")

file(WRITE "${fake_compat}" "#!/usr/bin/env bash
set -euo pipefail
bench_dir=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        --bench-dir) bench_dir=\"$2\"; shift 2 ;;
        --timeout) shift 2 ;;
        --limit) shift 2 ;;
        --runtime-lib) shift 2 ;;
        --lfortran) shift 2 ;;
        *) echo \"unexpected arg in fake bench_compat_check: $1\" >&2; exit 101 ;;
    esac
done
[[ -n \"$bench_dir\" ]] || { echo \"missing --bench-dir\" >&2; exit 102; }
cat > \"$bench_dir/compat_api.txt\" <<'TXT'
api_case_01
TXT
cat > \"$bench_dir/compat_ll.txt\" <<'TXT'
api_case_01
TXT
cat > \"$bench_dir/compat_ll_options.jsonl\" <<'JSONL'
{\"name\":\"api_case_01\",\"options\":\"\"}
JSONL
")

file(WRITE "${fake_api}" "#!/usr/bin/env bash
set -euo pipefail
bench_dir=''
policy=''
compat_list=''
options_jsonl=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        --bench-dir) bench_dir=\"$2\"; shift 2 ;;
        --timeout-ms|--min-completed|--fail-sample-limit) shift 2 ;;
        --require-zero-skips) shift 1 ;;
        --liric-policy) policy=\"$2\"; shift 2 ;;
        --compat-list) compat_list=\"$2\"; shift 2 ;;
        --options-jsonl) options_jsonl=\"$2\"; shift 2 ;;
        --lfortran|--lfortran-liric|--test-dir|--runtime-lib) shift 2 ;;
        *) echo \"unexpected arg in fake bench_api: $1\" >&2; exit 111 ;;
    esac
done
[[ -n \"$bench_dir\" ]] || { echo \"missing bench_dir\" >&2; exit 112; }
[[ \"$policy\" == \"direct\" ]] || { echo \"bad policy: $policy\" >&2; exit 113; }
[[ -n \"$compat_list\" ]] || { echo \"missing compat list\" >&2; exit 114; }
[[ -n \"$options_jsonl\" ]] || { echo \"missing options_jsonl\" >&2; exit 115; }
echo \"\${LIRIC_COMPILE_MODE:-unset}\" > \"${mode_log}\"
cat > \"$bench_dir/bench_api_summary.json\" <<'JSON'
{
  \"status\": \"OK\",
  \"attempted\": 1,
  \"completed\": 1,
  \"skipped\": 0,
  \"zero_skip_gate_met\": true
}
JSON
cat > \"$bench_dir/bench_api.jsonl\" <<'JSONL'
{\"name\":\"api_case_01\",\"status\":\"ok\",\"liric_wall_median_ms\":10.0,\"llvm_wall_median_ms\":12.0,\"liric_compile_median_ms\":2.0,\"llvm_compile_median_ms\":2.5,\"liric_run_median_ms\":1.0,\"llvm_run_median_ms\":1.2,\"liric_llvm_ir_median_ms\":0.6,\"llvm_llvm_ir_median_ms\":0.7,\"liric_backend_median_ms\":3.0,\"llvm_backend_median_ms\":3.7}
JSONL
")

file(WRITE "${fake_lfortran_llvm}" "#!/usr/bin/env bash
set -euo pipefail
exit 0
")

file(WRITE "${fake_lfortran_liric}" "#!/usr/bin/env bash
set -euo pipefail
exit 0
")

execute_process(COMMAND "${CHMOD_EXE}" +x
    "${fake_cmake}" "${fake_compat}" "${fake_api}"
    "${fake_lfortran_llvm}" "${fake_lfortran_liric}")

execute_process(
    COMMAND "${BENCH_MATRIX}"
        --bench-dir "${bench_dir}"
        --manifest "${manifest}"
        --modes isel
        --policies direct
        --lanes api_full_llvm
        --timeout 5
        --timeout-ms 1000
        --cmake "${fake_cmake}"
        --lfortran-build-dir "${llvm_build_dir}"
        --lfortran-liric-build-dir "${liric_build_dir}"
        --lfortran "${fake_lfortran_llvm}"
        --lfortran-liric "${fake_lfortran_liric}"
        --bench-compat-check "${fake_compat}"
        --bench-api "${fake_api}"
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "bench_matrix run failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

if(NOT EXISTS "${cmake_log}")
    message(FATAL_ERROR "fake cmake was not invoked")
endif()
if(NOT EXISTS "${mode_log}")
    message(FATAL_ERROR "fake bench_api was not invoked")
endif()

file(READ "${cmake_log}" cmake_text)
string(REGEX MATCHALL "--build" build_hits "${cmake_text}")
list(LENGTH build_hits build_count)
if(NOT build_count EQUAL 3)
    message(FATAL_ERROR "expected 3 cmake --build invocations, got ${build_count}\nlog:\n${cmake_text}")
endif()

string(FIND "${cmake_text}" "--build ${llvm_build_dir}" llvm_hit)
if(llvm_hit EQUAL -1)
    message(FATAL_ERROR "missing llvm rebuild dir in cmake args:\n${cmake_text}")
endif()
string(FIND "${cmake_text}" "--build ${liric_build_dir}" liric_hit)
if(liric_hit EQUAL -1)
    message(FATAL_ERROR "missing liric rebuild dir in cmake args:\n${cmake_text}")
endif()

file(READ "${mode_log}" mode_text)
if(NOT mode_text MATCHES "^isel")
    message(FATAL_ERROR "expected fake bench_api to run with mode=isel, got '${mode_text}'")
endif()

set(summary "${bench_dir}/matrix_summary.json")
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing matrix summary artifact: ${summary}")
endif()

file(READ "${summary}" summary_text)
if(NOT summary_text MATCHES "\"status\"[ \t]*:[ \t]*\"OK\"")
    message(FATAL_ERROR "matrix summary status not OK:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"cells_attempted\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "matrix summary attempted count mismatch:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"cells_ok\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "matrix summary ok count mismatch:\n${summary_text}")
endif()
if(NOT summary_text MATCHES "\"cells_failed\"[ \t]*:[ \t]*0")
    message(FATAL_ERROR "matrix summary failed count mismatch:\n${summary_text}")
endif()
