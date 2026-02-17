#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/bench_shell_common.sh
source "${script_dir}/bench_shell_common.sh"
BENCH_SCRIPT_NAME="bench_readme_perf_snapshot"

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
  bench_corpus_compare (single canonical corpus track)
USAGE
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
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            build_dir="$2"
            shift 2
            ;;
        --bench-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            bench_dir="$2"
            shift 2
            ;;
        --out-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            out_dir="$2"
            shift 2
            ;;
        --iters)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            iters="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            timeout_sec="$2"
            shift 2
            ;;
        --corpus)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            corpus_tsv="$2"
            shift 2
            ;;
        --cache-dir)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            cache_dir="$2"
            shift 2
            ;;
        --runtime-lib)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            runtime_lib="$2"
            shift 2
            ;;
        --lfortran-src)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
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
            bench_die "unknown argument: $1"
            ;;
    esac
done

repo_root="$(cd "${script_dir}/.." && pwd)"

if [[ -z "$runtime_lib" ]]; then
    runtime_lib="$(bench_find_runtime_lib "$lfortran_src" || true)"
fi

if [[ "$no_run" == "0" ]]; then
    bench_require_executable "${build_dir}/bench_corpus_compare"
    bench_require_executable "${build_dir}/liric_probe_runner"
    bench_require_executable "${build_dir}/bench_lli_phases"

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

summary_path="$(bench_to_abs_path "${bench_dir}/bench_corpus_compare_summary.json")"
jsonl_path="$(bench_to_abs_path "${bench_dir}/bench_corpus_compare.jsonl")"

bench_require_nonempty_file "$summary_path"
bench_require_nonempty_file "$jsonl_path"

status="$(bench_json_string_field "$summary_path" "status")"
dataset_name="$(bench_json_string_field "$summary_path" "dataset_name")"
expected_tests="$(bench_json_int_field "$summary_path" "expected_tests")"
attempted_tests="$(bench_json_int_field "$summary_path" "attempted_tests")"
completed_tests="$(bench_json_int_field "$summary_path" "completed_tests")"
summary_iters="$(bench_json_int_field "$summary_path" "iters")"

[[ "$dataset_name" == "corpus_100" ]] || bench_die "dataset_name must be corpus_100 (got '${dataset_name}')"
[[ "$expected_tests" -eq 100 ]] || bench_die "expected_tests must be 100 (got '${expected_tests}')"
[[ "$attempted_tests" -gt 0 ]] || bench_die "attempted_tests must be > 0"
[[ "$summary_iters" -gt 0 ]] || bench_die "iters must be > 0"

liric_parse="$(bench_json_number_field "$summary_path" "liric_parse_median_ms")"
liric_compile_mat="$(bench_json_number_field "$summary_path" "liric_compile_materialized_median_ms")"
liric_total_mat="$(bench_json_number_field "$summary_path" "liric_total_materialized_median_ms")"
llvm_parse="$(bench_json_number_field "$summary_path" "llvm_parse_median_ms")"
llvm_compile_mat="$(bench_json_number_field "$summary_path" "llvm_compile_materialized_median_ms")"
llvm_total_mat="$(bench_json_number_field "$summary_path" "llvm_total_materialized_median_ms")"
speedup_nonparse_median="$(bench_json_number_field "$summary_path" "compile_materialized_speedup_median")"
speedup_nonparse_agg="$(bench_json_number_field "$summary_path" "compile_materialized_speedup_aggregate")"
speedup_total_median="$(bench_json_number_field "$summary_path" "total_materialized_speedup_median")"
speedup_total_agg="$(bench_json_number_field "$summary_path" "total_materialized_speedup_aggregate")"

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
out_abs="$(bench_to_abs_path "$out_dir")"
snapshot_path="${out_abs}/readme_perf_snapshot.json"
table_path="${out_abs}/readme_perf_table.md"

cat > "$snapshot_path" <<EOF_JSON
{
  "schema_version": 3,
  "generated_at_utc": "$(bench_json_escape "$generated_at_utc")",
  "benchmark_commit": "$(bench_json_escape "$benchmark_commit")",
  "host": {
    "kernel": "$(bench_json_escape "$host_kernel")",
    "cpu": "$(bench_json_escape "$host_cpu")"
  },
  "toolchain": {
    "cc": "$(bench_json_escape "$cc_version")",
    "lli": "$(bench_json_escape "$lli_version")"
  },
  "status": "$(bench_json_escape "$status")",
  "dataset_name": "$(bench_json_escape "$dataset_name")",
  "expected_tests": ${expected_tests},
  "attempted_tests": ${attempted_tests},
  "iters": ${summary_iters},
  "canonical_track": "corpus_canonical",
  "completed_tests": ${completed_tests},
  "liric_parse_median_ms": ${liric_parse},
  "liric_compile_materialized_median_ms": ${liric_compile_mat},
  "liric_total_materialized_median_ms": ${liric_total_mat},
  "llvm_parse_median_ms": ${llvm_parse},
  "llvm_compile_materialized_median_ms": ${llvm_compile_mat},
  "llvm_total_materialized_median_ms": ${llvm_total_mat},
  "compile_materialized_speedup_median": ${speedup_nonparse_median},
  "compile_materialized_speedup_aggregate": ${speedup_nonparse_agg},
  "total_materialized_speedup_median": ${speedup_total_median},
  "total_materialized_speedup_aggregate": ${speedup_total_agg},
  "artifacts": {
    "bench_corpus_compare_summary_json": "$(bench_json_escape "$summary_path")",
    "bench_corpus_compare_jsonl": "$(bench_json_escape "$jsonl_path")",
    "published_snapshot_json": "$(bench_json_escape "$snapshot_path")",
    "published_table_md": "$(bench_json_escape "$table_path")"
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
Canonical track: corpus_canonical (${status}; completed ${completed_tests}/${attempted_tests})

Artifacts:
- ${summary_path}
- ${jsonl_path}
- ${snapshot_path}

Legend: canonical corpus lane only; no duplicate tracks.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| corpus_canonical (canonical) | ${completed_tests}/${attempted_tests} | $(bench_fmt_fixed "$liric_parse" 3) | $(bench_fmt_fixed "$liric_compile_mat" 3) | $(bench_fmt_fixed "$liric_total_mat" 3) | $(bench_fmt_fixed "$llvm_parse" 3) | $(bench_fmt_fixed "$llvm_compile_mat" 3) | $(bench_fmt_fixed "$llvm_total_mat" 3) | $(bench_fmt_fixed "$speedup_nonparse_median" 2)x | $(bench_fmt_fixed "$speedup_nonparse_agg" 2)x | $(bench_fmt_fixed "$speedup_total_median" 2)x | $(bench_fmt_fixed "$speedup_total_agg" 2)x |
EOF_MD

echo "Published snapshot: ${snapshot_path}"
echo "Published table: ${table_path}"
