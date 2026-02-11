#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage: bench_api_clean_gate.sh [options]
  --build-dir PATH        build dir containing bench tools (default: ./build)
  --bench-dir PATH        benchmark output dir (default: /tmp/liric_bench)
  --iters N               bench_api iterations (default: 1)
  --timeout-ms N          bench_api timeout in ms (default: 3000)
  --compat-timeout N      bench_compat_check timeout in seconds (default: 15)
  --no-run                only validate existing summary artifacts in bench-dir
  -h, --help              show this help

Gate conditions (must all be true):
  - bench_api_summary.json: skipped == 0
  - bench_api_summary.json: attempted == completed
  - bench_api_summary.json: zero_skip_gate_met == true
  - bench_api_fail_summary.json: failed == 0
EOF
}

die() {
    echo "bench_api_clean_gate: $*" >&2
    exit 1
}

json_int_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -E "\"${key}\"[[:space:]]*:" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing integer field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/'
}

json_bool_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -E "\"${key}\"[[:space:]]*:" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing boolean field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*(true|false).*/\1/'
}

build_dir="./build"
bench_dir="/tmp/liric_bench"
iters="1"
timeout_ms="3000"
compat_timeout="15"
no_run="0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            build_dir="$2"
            shift 2
            ;;
        --bench-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            bench_dir="$2"
            shift 2
            ;;
        --iters)
            [[ $# -ge 2 ]] || die "missing value for $1"
            iters="$2"
            shift 2
            ;;
        --timeout-ms)
            [[ $# -ge 2 ]] || die "missing value for $1"
            timeout_ms="$2"
            shift 2
            ;;
        --compat-timeout)
            [[ $# -ge 2 ]] || die "missing value for $1"
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
            die "unknown argument: $1"
            ;;
    esac
done

if [[ "$no_run" == "0" ]]; then
    [[ -x "${build_dir}/bench_compat_check" ]] || die "missing executable: ${build_dir}/bench_compat_check"
    [[ -x "${build_dir}/bench_api" ]] || die "missing executable: ${build_dir}/bench_api"

    "${build_dir}/bench_compat_check" --timeout "$compat_timeout" --bench-dir "$bench_dir"
    "${build_dir}/bench_api" \
        --iters "$iters" \
        --timeout-ms "$timeout_ms" \
        --bench-dir "$bench_dir" \
        --require-zero-skips
fi

summary_path="${bench_dir}/bench_api_summary.json"
fail_summary_path="${bench_dir}/bench_api_fail_summary.json"

[[ -s "$summary_path" ]] || die "missing summary artifact: ${summary_path}"
[[ -s "$fail_summary_path" ]] || die "missing failure summary artifact: ${fail_summary_path}"

attempted="$(json_int_field "$summary_path" "attempted")"
completed="$(json_int_field "$summary_path" "completed")"
skipped="$(json_int_field "$summary_path" "skipped")"
zero_skip_gate_met="$(json_bool_field "$summary_path" "zero_skip_gate_met")"
failed="$(json_int_field "$fail_summary_path" "failed")"

errors=()
if [[ "$skipped" != "0" ]]; then
    errors+=("skipped=${skipped} (expected 0)")
fi
if [[ "$attempted" != "$completed" ]]; then
    errors+=("attempted=${attempted} completed=${completed} (expected equality)")
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
echo "  failed=${failed}"
echo "  zero_skip_gate_met=${zero_skip_gate_met}"

