if(NOT DEFINED GATE_SCRIPT OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "GATE_SCRIPT and WORKDIR are required")
endif()

if(NOT EXISTS "${GATE_SCRIPT}")
    message(FATAL_ERROR "gate script not found: ${GATE_SCRIPT}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping bench_api_clean_gate forwarding test")
    return()
endif()

set(root "${WORKDIR}/bench_api_clean_gate_forwarding")
set(fake_build "${root}/fake_build")
set(bench_dir "${root}/bench")
set(log_dir "${root}/logs")
set(compat_log "${log_dir}/compat_args.log")
set(api_log "${log_dir}/api_args.log")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${fake_build}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${log_dir}")

set(fake_compat "${fake_build}/bench_compat_check")
set(fake_api "${fake_build}/bench_api")

file(WRITE "${fake_compat}" "#!/usr/bin/env bash
set -euo pipefail
timeout_val=''
bench_dir=''
lfortran=''
probe=''
runtime=''
cmake_path=''
workers=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        --timeout) timeout_val=\"$2\"; shift 2 ;;
        --bench-dir) bench_dir=\"$2\"; shift 2 ;;
        --lfortran) lfortran=\"$2\"; shift 2 ;;
        --probe-runner) probe=\"$2\"; shift 2 ;;
        --runtime-lib) runtime=\"$2\"; shift 2 ;;
        --cmake) cmake_path=\"$2\"; shift 2 ;;
        --workers) workers=\"$2\"; shift 2 ;;
        *) echo \"unexpected arg in fake bench_compat_check: $1\" >&2; exit 90 ;;
    esac
done
[[ \"$timeout_val\" == \"17\" ]] || { echo \"bad timeout: $timeout_val\" >&2; exit 91; }
[[ \"$bench_dir\" == \"${bench_dir}\" ]] || { echo \"bad bench_dir: $bench_dir\" >&2; exit 92; }
[[ \"$lfortran\" == \"/opt/lfortran-llvm/bin/lfortran\" ]] || { echo \"bad lfortran: $lfortran\" >&2; exit 93; }
[[ \"$probe\" == \"/opt/liric/bin/liric_probe_runner\" ]] || { echo \"bad probe: $probe\" >&2; exit 94; }
[[ \"$runtime\" == \"/opt/lfortran/lib/liblfortran_runtime.so\" ]] || { echo \"bad runtime: $runtime\" >&2; exit 95; }
[[ \"$cmake_path\" == \"/opt/lfortran/integration_tests/CMakeLists.txt\" ]] || { echo \"bad cmake: $cmake_path\" >&2; exit 99; }
[[ \"$workers\" == \"11\" ]] || { echo \"bad workers: $workers\" >&2; exit 98; }
echo \"ok\" > \"${compat_log}\"
")

file(WRITE "${fake_api}" "#!/usr/bin/env bash
set -euo pipefail
iters=''
timeout_ms=''
bench_dir=''
require_zero='0'
lfortran=''
lfortran_liric=''
test_dir=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        --iters) iters=\"$2\"; shift 2 ;;
        --timeout-ms) timeout_ms=\"$2\"; shift 2 ;;
        --bench-dir) bench_dir=\"$2\"; shift 2 ;;
        --require-zero-skips) require_zero='1'; shift 1 ;;
        --lfortran) lfortran=\"$2\"; shift 2 ;;
        --lfortran-liric) lfortran_liric=\"$2\"; shift 2 ;;
        --test-dir) test_dir=\"$2\"; shift 2 ;;
        *) echo \"unexpected arg in fake bench_api: $1\" >&2; exit 110 ;;
    esac
done
[[ \"$iters\" == \"2\" ]] || { echo \"bad iters: $iters\" >&2; exit 111; }
[[ \"$timeout_ms\" == \"4321\" ]] || { echo \"bad timeout_ms: $timeout_ms\" >&2; exit 112; }
[[ \"$bench_dir\" == \"${bench_dir}\" ]] || { echo \"bad bench_dir: $bench_dir\" >&2; exit 113; }
[[ \"$require_zero\" == \"1\" ]] || { echo \"missing --require-zero-skips\" >&2; exit 114; }
[[ \"$lfortran\" == \"/opt/lfortran-llvm/bin/lfortran\" ]] || { echo \"bad lfortran: $lfortran\" >&2; exit 115; }
[[ \"$lfortran_liric\" == \"/opt/lfortran-liric/bin/lfortran\" ]] || { echo \"bad lfortran_liric: $lfortran_liric\" >&2; exit 116; }
[[ \"$test_dir\" == \"/opt/lfortran/integration_tests\" ]] || { echo \"bad test_dir: $test_dir\" >&2; exit 117; }
mkdir -p \"$bench_dir\"
cat > \"$bench_dir/bench_api_summary.json\" <<'JSON'
{
  \"attempted\": 5,
  \"completed\": 5,
  \"skipped\": 0,
  \"timeout_ms\": 4321,
  \"skip_reasons\": {
    \"liric_jit_timeout\": 0
  },
  \"zero_skip_gate_met\": true
}
JSON
cat > \"$bench_dir/bench_api_fail_summary.json\" <<'JSON'
{
  \"failed\": 0
}
JSON
echo \"ok\" > \"${api_log}\"
")

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for bench_api_clean_gate forwarding test")
endif()
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_compat}" "${fake_api}")

execute_process(
    COMMAND "${BASH_EXE}" "${GATE_SCRIPT}"
        --build-dir "${fake_build}"
        --bench-dir "${bench_dir}"
        --iters 2
        --timeout-ms 4321
        --compat-timeout 17
        --lfortran /opt/lfortran-llvm/bin/lfortran
        --lfortran-liric /opt/lfortran-liric/bin/lfortran
        --test-dir /opt/lfortran/integration_tests
        --probe-runner /opt/liric/bin/liric_probe_runner
        --runtime-lib /opt/lfortran/lib/liblfortran_runtime.so
        --cmake /opt/lfortran/integration_tests/CMakeLists.txt
        --workers 11
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "bench_api_clean_gate forwarding run failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

if(NOT out MATCHES "bench_api_clean_gate: PASSED")
    message(FATAL_ERROR "gate output missing PASSED marker\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT EXISTS "${compat_log}")
    message(FATAL_ERROR "fake bench_compat_check was not invoked")
endif()
if(NOT EXISTS "${api_log}")
    message(FATAL_ERROR "fake bench_api was not invoked")
endif()
