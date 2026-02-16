#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/bench_shell_common.sh
source "${script_dir}/bench_shell_common.sh"
BENCH_SCRIPT_NAME="bench_api_clean_gate"

usage() {
    cat <<'EOF'
usage: bench_api_clean_gate.sh [options]
  --build-dir PATH        build dir containing bench tools (default: ./build)
  --bench-dir PATH        benchmark output dir (default: /tmp/liric_bench)
  --lfortran PATH         path to lfortran+LLVM binary (forwarded to tools)
  --lfortran-liric PATH   path to lfortran+WITH_LIRIC binary (forwarded to bench_lane_api)
  --test-dir PATH         path to integration_tests dir (forwarded to bench_api)
  --probe-runner PATH     path to liric_probe_runner (forwarded to bench_compat_check)
  --runtime-lib PATH      path to liblfortran_runtime for lli (forwarded to bench_compat_check)
  --liric-compile-mode M  compile mode for liric backend in bench_lane_api (default: isel)
  --cmake PATH            path to integration_tests/CMakeLists.txt (forwarded to bench_compat_check)
  --workers N             worker count hint (forwarded to bench_compat_check)
  --iters N               bench_lane_api iterations (default: 1)
  --timeout-ms N          bench_lane_api timeout in ms (default: 3000)
  --compat-timeout N      bench_compat_check timeout in seconds (default: 15)
  --no-run                only validate existing summary artifacts in bench-dir
  -h, --help              show this help

Gate conditions (must all be true):
  - bench_api_summary.json: skipped == 0
  - bench_api_summary.json: attempted == completed
  - bench_api_summary.json: timeout_ms == --timeout-ms
  - bench_api_summary.json: skip_reasons.liric_jit_timeout == 0
  - bench_api_summary.json: zero_skip_gate_met == true
  - bench_api_fail_summary.json: failed == 0
EOF
}

build_dir="./build"
bench_dir="/tmp/liric_bench"
lfortran_path=""
lfortran_liric_path=""
test_dir=""
probe_runner=""
runtime_lib=""
liric_compile_mode="isel"
cmake_path=""
workers=""
iters="1"
timeout_ms="3000"
compat_timeout="15"
no_run="0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            build_dir="$2"
            shift 2
            ;;
        --bench-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            bench_dir="$2"
            shift 2
            ;;
        --lfortran)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            lfortran_path="$2"
            shift 2
            ;;
        --lfortran-liric)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            lfortran_liric_path="$2"
            shift 2
            ;;
        --test-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            test_dir="$2"
            shift 2
            ;;
        --probe-runner)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            probe_runner="$2"
            shift 2
            ;;
        --runtime-lib)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            runtime_lib="$2"
            shift 2
            ;;
        --liric-compile-mode)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            liric_compile_mode="$2"
            shift 2
            ;;
        --cmake)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            cmake_path="$2"
            shift 2
            ;;
        --workers)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            workers="$2"
            shift 2
            ;;
        --iters)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            iters="$2"
            shift 2
            ;;
        --timeout-ms)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            timeout_ms="$2"
            shift 2
            ;;
        --compat-timeout)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            compat_timeout="$2"
            shift 2
            ;;
        --no-run)
            no_run="1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            bench_die "unknown argument: $1"
            ;;
    esac
done

if [[ "$no_run" == "0" ]]; then
    compat_args=(
        --timeout "$compat_timeout"
        --bench-dir "$bench_dir"
    )
    api_args=(
        --iters "$iters"
        --timeout-ms "$timeout_ms"
        --bench-dir "$bench_dir"
        --liric-compile-mode "$liric_compile_mode"
        --require-zero-skips
    )

    if [[ -n "$lfortran_path" ]]; then
        compat_args+=(--lfortran "$lfortran_path")
        api_args+=(--lfortran "$lfortran_path")
    fi
    if [[ -n "$lfortran_liric_path" ]]; then
        api_args+=(--lfortran-liric "$lfortran_liric_path")
    fi
    if [[ -n "$test_dir" ]]; then
        api_args+=(--test-dir "$test_dir")
    fi
    if [[ -n "$probe_runner" ]]; then
        compat_args+=(--probe-runner "$probe_runner")
    fi
    if [[ -n "$runtime_lib" ]]; then
        compat_args+=(--runtime-lib "$runtime_lib")
    fi
    if [[ -n "$cmake_path" ]]; then
        compat_args+=(--cmake "$cmake_path")
    fi
    if [[ -n "$workers" ]]; then
        compat_args+=(--workers "$workers")
    fi

    bench_require_executable "${build_dir}/bench_compat_check"
    bench_require_executable "${build_dir}/bench_lane_api"

    "${build_dir}/bench_compat_check" "${compat_args[@]}"
    "${build_dir}/bench_lane_api" "${api_args[@]}"
fi

summary_path="${bench_dir}/bench_api_summary.json"
fail_summary_path="${bench_dir}/bench_api_fail_summary.json"

bench_require_nonempty_file "$summary_path"
bench_require_nonempty_file "$fail_summary_path"

attempted="$(bench_json_int_field "$summary_path" "attempted")"
completed="$(bench_json_int_field "$summary_path" "completed")"
skipped="$(bench_json_int_field "$summary_path" "skipped")"
timeout_ms_used="$(bench_json_int_field "$summary_path" "timeout_ms")"
liric_jit_timeout="$(bench_json_int_field "$summary_path" "liric_jit_timeout")"
zero_skip_gate_met="$(bench_json_bool_field "$summary_path" "zero_skip_gate_met")"
failed="$(bench_json_int_field "$fail_summary_path" "failed")"

errors=()
if [[ "$skipped" != "0" ]]; then
    errors+=("skipped=${skipped} (expected 0)")
fi
if [[ "$attempted" != "$completed" ]]; then
    errors+=("attempted=${attempted} completed=${completed} (expected equality)")
fi
if [[ "$timeout_ms_used" != "$timeout_ms" ]]; then
    errors+=("timeout_ms=${timeout_ms_used} (expected ${timeout_ms})")
fi
if [[ "$liric_jit_timeout" != "0" ]]; then
    errors+=("skip_reasons.liric_jit_timeout=${liric_jit_timeout} (expected 0)")
fi
if [[ "$failed" != "0" ]]; then
    errors+=("failed=${failed} (expected 0)")
fi
if [[ "$zero_skip_gate_met" != "true" ]]; then
    errors+=("zero_skip_gate_met=${zero_skip_gate_met} (expected true)")
fi

if [[ "${#errors[@]}" -gt 0 ]]; then
    echo "bench_api_clean_gate: FAILED" >&2
    for err in "${errors[@]}"; do
        echo "  - ${err}" >&2
    done
    echo "  summary: ${summary_path}" >&2
    echo "  fail summary: ${fail_summary_path}" >&2
    exit 1
fi

echo "bench_api_clean_gate: PASSED"
echo "  attempted=${attempted}"
echo "  completed=${completed}"
echo "  skipped=${skipped}"
echo "  timeout_ms=${timeout_ms_used}"
echo "  skip_reasons.liric_jit_timeout=${liric_jit_timeout}"
echo "  failed=${failed}"
echo "  zero_skip_gate_met=${zero_skip_gate_met}"
