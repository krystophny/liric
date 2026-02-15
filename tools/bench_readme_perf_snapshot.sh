#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: bench_readme_perf_snapshot.sh [options]
  --build-dir PATH       build dir containing benchmark tools (default: ./build)
  --bench-dir PATH       benchmark output dir (default: /tmp/liric_bench)
  --out-dir PATH         output dir for published artifacts (default: docs/benchmarks)
  --iters N              iterations for corpus comparator (default: 3)
  --timeout N            timeout seconds per command (default: 30)
  --corpus PATH          corpus TSV (default: tools/corpus_100.tsv)
  --cache-dir PATH       corpus cache dir (default: /tmp/liric_lfortran_mass/cache)
  --runtime-lib PATH     runtime shared library path for core track (auto-detect by default)
  --lfortran-src PATH    lfortran source root for runtime-lib auto-detect (default: ../lfortran)
  --no-run               do not execute benchmarks; consume existing comparator artifacts
  -h, --help             show this help

Outputs:
  <out-dir>/readme_perf_snapshot.json
  <out-dir>/readme_perf_table.md

Default run mode executes:
  bench_corpus_compare (dual track: core + runtime_equalized_bc)
USAGE
}

die() {
    echo "bench_readme_perf_snapshot: $*" >&2
    exit 1
}

json_number_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*-?[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing numeric field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*(-?[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?).*/\1/'
}

json_int_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*[0-9]+" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing integer field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/'
}

json_string_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing string field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*"([^"]*)".*/\1/'
}

json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

to_abs_path() {
    local p="$1"
    if command -v realpath >/dev/null 2>&1; then
        realpath "$p"
        return
    fi
    if [[ "$p" == /* ]]; then
        printf '%s\n' "$p"
        return
    fi
    printf '%s/%s\n' "$(pwd)" "$p"
}

fmt_fixed() {
    local val="$1"
    local digits="$2"
    awk -v v="$val" -v d="$digits" 'BEGIN { printf("%.*f", d, v + 0.0) }'
}

build_dir="./build"
bench_dir="/tmp/liric_bench"
out_dir="docs/benchmarks"
iters="3"
timeout_sec="30"
corpus_tsv="tools/corpus_100.tsv"
cache_dir="/tmp/liric_lfortran_mass/cache"
runtime_lib=""
lfortran_src="../lfortran"
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
        --out-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            out_dir="$2"
            shift 2
            ;;
        --iters)
            [[ $# -ge 2 ]] || die "missing value for $1"
            iters="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "missing value for $1"
            timeout_sec="$2"
            shift 2
            ;;
        --corpus)
            [[ $# -ge 2 ]] || die "missing value for $1"
            corpus_tsv="$2"
            shift 2
            ;;
        --cache-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            cache_dir="$2"
            shift 2
            ;;
        --runtime-lib)
            [[ $# -ge 2 ]] || die "missing value for $1"
            runtime_lib="$2"
            shift 2
            ;;
        --lfortran-src)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_src="$2"
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

if [[ -z "$runtime_lib" ]]; then
    for cand in \
        "${lfortran_src}/build/src/runtime/liblfortran_runtime.so" \
        "${lfortran_src}/build/src/runtime/liblfortran_runtime.so."* \
        "${lfortran_src}/build/src/runtime/liblfortran_runtime.dylib" \
        "${lfortran_src}/build-llvm-release/src/runtime/liblfortran_runtime.so" \
        "${lfortran_src}/build-llvm-release/src/runtime/liblfortran_runtime.so."* \
        "${lfortran_src}/build-llvm-release/src/runtime/liblfortran_runtime.dylib" \
        "${lfortran_src}/build-liric/src/runtime/liblfortran_runtime.so" \
        "${lfortran_src}/build-liric/src/runtime/liblfortran_runtime.so."* \
        "${lfortran_src}/build-liric/src/runtime/liblfortran_runtime.dylib" \
        "${lfortran_src}/build_liric/src/runtime/liblfortran_runtime.so" \
        "${lfortran_src}/build_liric/src/runtime/liblfortran_runtime.so."* \
        "${lfortran_src}/build_liric/src/runtime/liblfortran_runtime.dylib"; do
        if [[ -f "$cand" ]]; then
            runtime_lib="$cand"
            break
        fi
    done
fi

if [[ "$no_run" == "0" ]]; then
    [[ -x "${build_dir}/bench_corpus_compare" ]] || die "missing executable: ${build_dir}/bench_corpus_compare"
    [[ -x "${build_dir}/liric_probe_runner" ]] || die "missing executable: ${build_dir}/liric_probe_runner"
    [[ -x "${build_dir}/bench_lli_phases" ]] || die "missing executable: ${build_dir}/bench_lli_phases"

    run_cmd=(
        "${build_dir}/bench_corpus_compare"
        --probe-runner "${build_dir}/liric_probe_runner"
        --lli-phases "${build_dir}/bench_lli_phases"
        --corpus "$corpus_tsv"
        --cache-dir "$cache_dir"
        --bench-dir "$bench_dir"
        --iters "$iters"
        --timeout "$timeout_sec"
    )
    if [[ -n "$runtime_lib" ]]; then
        run_cmd+=(--runtime-lib "$runtime_lib")
    fi
    "${run_cmd[@]}"
fi

summary_path="$(to_abs_path "${bench_dir}/bench_corpus_compare_summary.json")"
core_jsonl="$(to_abs_path "${bench_dir}/bench_corpus_compare_core.jsonl")"
runtime_jsonl="$(to_abs_path "${bench_dir}/bench_corpus_compare_runtime_equalized_bc.jsonl")"

[[ -s "$summary_path" ]] || die "missing artifact: ${summary_path}"
[[ -e "$core_jsonl" ]] || die "missing artifact: ${core_jsonl}"
[[ -s "$runtime_jsonl" ]] || die "missing artifact: ${runtime_jsonl}"

status="$(json_string_field "$summary_path" "status")"
dataset_name="$(json_string_field "$summary_path" "dataset_name")"
expected_tests="$(json_int_field "$summary_path" "expected_tests")"
attempted_tests="$(json_int_field "$summary_path" "attempted_tests")"
summary_iters="$(json_int_field "$summary_path" "iters")"
core_completed="$(json_int_field "$summary_path" "core_completed")"
runtime_completed="$(json_int_field "$summary_path" "runtime_equalized_bc_completed")"

[[ "$dataset_name" == "corpus_100" ]] || die "dataset_name must be corpus_100 (got '${dataset_name}')"
[[ "$expected_tests" -eq 100 ]] || die "expected_tests must be 100 (got '${expected_tests}')"
[[ "$attempted_tests" -gt 0 ]] || die "attempted_tests must be > 0"
[[ "$summary_iters" -gt 0 ]] || die "iters must be > 0"

core_liric_parse="$(json_number_field "$summary_path" "core_liric_parse_median_ms")"
core_liric_compile_mat="$(json_number_field "$summary_path" "core_liric_compile_materialized_median_ms")"
core_liric_total_mat="$(json_number_field "$summary_path" "core_liric_total_materialized_median_ms")"
core_llvm_parse="$(json_number_field "$summary_path" "core_llvm_parse_median_ms")"
core_llvm_compile_mat="$(json_number_field "$summary_path" "core_llvm_compile_materialized_median_ms")"
core_llvm_total_mat="$(json_number_field "$summary_path" "core_llvm_total_materialized_median_ms")"
core_speedup_nonparse_median="$(json_number_field "$summary_path" "core_compile_materialized_speedup_median")"
core_speedup_nonparse_agg="$(json_number_field "$summary_path" "core_compile_materialized_speedup_aggregate")"
core_speedup_total_median="$(json_number_field "$summary_path" "core_total_materialized_speedup_median")"
core_speedup_total_agg="$(json_number_field "$summary_path" "core_total_materialized_speedup_aggregate")"

rt_liric_parse="$(json_number_field "$summary_path" "runtime_equalized_bc_liric_parse_median_ms")"
rt_liric_compile_mat="$(json_number_field "$summary_path" "runtime_equalized_bc_liric_compile_materialized_median_ms")"
rt_liric_total_mat="$(json_number_field "$summary_path" "runtime_equalized_bc_liric_total_materialized_median_ms")"
rt_llvm_parse="$(json_number_field "$summary_path" "runtime_equalized_bc_llvm_parse_median_ms")"
rt_llvm_compile_mat="$(json_number_field "$summary_path" "runtime_equalized_bc_llvm_compile_materialized_median_ms")"
rt_llvm_total_mat="$(json_number_field "$summary_path" "runtime_equalized_bc_llvm_total_materialized_median_ms")"
rt_speedup_nonparse_median="$(json_number_field "$summary_path" "runtime_equalized_bc_compile_materialized_speedup_median")"
rt_speedup_nonparse_agg="$(json_number_field "$summary_path" "runtime_equalized_bc_compile_materialized_speedup_aggregate")"
rt_speedup_total_median="$(json_number_field "$summary_path" "runtime_equalized_bc_total_materialized_speedup_median")"
rt_speedup_total_agg="$(json_number_field "$summary_path" "runtime_equalized_bc_total_materialized_speedup_aggregate")"

runtime_status="partial"
if [[ "$runtime_completed" -eq "$expected_tests" ]]; then
    runtime_status="complete"
fi

generated_at_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
benchmark_commit="$(git -C "$repo_root" rev-parse HEAD)"
host_kernel="$(uname -srmo 2>/dev/null || uname -a)"
if [[ -r /proc/cpuinfo ]]; then
    host_cpu="$(awk -F': *' '/model name/{print $2; exit}' /proc/cpuinfo)"
else
    host_cpu="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")"
fi
cc_version="$("${CC:-cc}" --version 2>/dev/null | head -n 1 || true)"
if [[ -z "$cc_version" ]]; then
    cc_version="unavailable"
fi
lli_version="$(lli --version 2>/dev/null | sed -n '2p' | sed -E 's/^[[:space:]]+//' || true)"
if [[ -z "$lli_version" ]]; then
    lli_version="$(lli --version 2>/dev/null | head -n 1 || true)"
fi
if [[ -z "$lli_version" ]]; then
    lli_version="unavailable"
fi

mkdir -p "$out_dir"
out_abs="$(to_abs_path "$out_dir")"
snapshot_path="${out_abs}/readme_perf_snapshot.json"
table_path="${out_abs}/readme_perf_table.md"

cat > "$snapshot_path" <<EOF_JSON
{
  "schema_version": 3,
  "generated_at_utc": "$(json_escape "$generated_at_utc")",
  "benchmark_commit": "$(json_escape "$benchmark_commit")",
  "host": {
    "kernel": "$(json_escape "$host_kernel")",
    "cpu": "$(json_escape "$host_cpu")"
  },
  "toolchain": {
    "cc": "$(json_escape "$cc_version")",
    "lli": "$(json_escape "$lli_version")"
  },
  "status": "$(json_escape "$status")",
  "dataset_name": "$(json_escape "$dataset_name")",
  "expected_tests": ${expected_tests},
  "attempted_tests": ${attempted_tests},
  "iters": ${summary_iters},
  "canonical_track": "runtime_equalized_bc",
  "core_completed_tests": ${core_completed},
  "runtime_equalized_bc_completed_tests": ${runtime_completed},
  "core_liric_parse_median_ms": ${core_liric_parse},
  "core_liric_compile_materialized_median_ms": ${core_liric_compile_mat},
  "core_liric_total_materialized_median_ms": ${core_liric_total_mat},
  "core_llvm_parse_median_ms": ${core_llvm_parse},
  "core_llvm_compile_materialized_median_ms": ${core_llvm_compile_mat},
  "core_llvm_total_materialized_median_ms": ${core_llvm_total_mat},
  "core_compile_materialized_speedup_median": ${core_speedup_nonparse_median},
  "core_compile_materialized_speedup_aggregate": ${core_speedup_nonparse_agg},
  "core_total_materialized_speedup_median": ${core_speedup_total_median},
  "core_total_materialized_speedup_aggregate": ${core_speedup_total_agg},
  "runtime_equalized_bc_liric_parse_median_ms": ${rt_liric_parse},
  "runtime_equalized_bc_liric_compile_materialized_median_ms": ${rt_liric_compile_mat},
  "runtime_equalized_bc_liric_total_materialized_median_ms": ${rt_liric_total_mat},
  "runtime_equalized_bc_llvm_parse_median_ms": ${rt_llvm_parse},
  "runtime_equalized_bc_llvm_compile_materialized_median_ms": ${rt_llvm_compile_mat},
  "runtime_equalized_bc_llvm_total_materialized_median_ms": ${rt_llvm_total_mat},
  "runtime_equalized_bc_compile_materialized_speedup_median": ${rt_speedup_nonparse_median},
  "runtime_equalized_bc_compile_materialized_speedup_aggregate": ${rt_speedup_nonparse_agg},
  "runtime_equalized_bc_total_materialized_speedup_median": ${rt_speedup_total_median},
  "runtime_equalized_bc_total_materialized_speedup_aggregate": ${rt_speedup_total_agg},
  "artifacts": {
    "bench_corpus_compare_summary_json": "$(json_escape "$summary_path")",
    "bench_corpus_compare_core_jsonl": "$(json_escape "$core_jsonl")",
    "bench_corpus_compare_runtime_equalized_bc_jsonl": "$(json_escape "$runtime_jsonl")",
    "published_snapshot_json": "$(json_escape "$snapshot_path")",
    "published_table_md": "$(json_escape "$table_path")"
  }
}
EOF_JSON

cat > "$table_path" <<EOF_MD
# README Performance Snapshot

Generated: ${generated_at_utc}
Benchmark commit: ${benchmark_commit}
Host: ${host_cpu} (${host_kernel})
Toolchain: ${cc_version}; ${lli_version}
Dataset: ${dataset_name} (expected ${expected_tests}, attempted ${attempted_tests}, iters ${summary_iters})
Canonical track: runtime_equalized (${runtime_status}; completed ${runtime_completed}/${expected_tests})

Artifacts:
- ${summary_path}
- ${core_jsonl}
- ${runtime_jsonl}
- ${snapshot_path}

Legend: `input_only` excludes runtime-bc parse/merge overhead. `runtime_equalized` includes runtime-bc on both sides.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| input_only | ${core_completed}/${attempted_tests} | $(fmt_fixed "$core_liric_parse" 3) | $(fmt_fixed "$core_liric_compile_mat" 3) | $(fmt_fixed "$core_liric_total_mat" 3) | $(fmt_fixed "$core_llvm_parse" 3) | $(fmt_fixed "$core_llvm_compile_mat" 3) | $(fmt_fixed "$core_llvm_total_mat" 3) | $(fmt_fixed "$core_speedup_nonparse_median" 2)x | $(fmt_fixed "$core_speedup_nonparse_agg" 2)x | $(fmt_fixed "$core_speedup_total_median" 2)x | $(fmt_fixed "$core_speedup_total_agg" 2)x |
| runtime_equalized (canonical) | ${runtime_completed}/${expected_tests} | $(fmt_fixed "$rt_liric_parse" 3) | $(fmt_fixed "$rt_liric_compile_mat" 3) | $(fmt_fixed "$rt_liric_total_mat" 3) | $(fmt_fixed "$rt_llvm_parse" 3) | $(fmt_fixed "$rt_llvm_compile_mat" 3) | $(fmt_fixed "$rt_llvm_total_mat" 3) | $(fmt_fixed "$rt_speedup_nonparse_median" 2)x | $(fmt_fixed "$rt_speedup_nonparse_agg" 2)x | $(fmt_fixed "$rt_speedup_total_median" 2)x | $(fmt_fixed "$rt_speedup_total_agg" 2)x |
EOF_MD

echo "Published snapshot: ${snapshot_path}"
echo "Published table: ${table_path}"
