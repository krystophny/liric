#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: bench_readme_perf_snapshot.sh [options]
  --build-dir PATH       build dir containing bench tools (default: ./build)
  --bench-dir PATH       benchmark output dir (default: /tmp/liric_bench)
  --out-dir PATH         output dir for published artifacts (default: docs/benchmarks)
  --iters N              iterations for bench_ll (default: 3)
  --compat-timeout N     timeout seconds for bench_compat_check (default: 15)
  --no-run               do not execute benchmarks; consume existing artifacts
  -h, --help             show this help

Outputs:
  <out-dir>/readme_perf_snapshot.json
  <out-dir>/readme_perf_table.md

Default run mode executes:
  1) bench_compat_check --timeout <compat-timeout>
  2) bench_ll --iters <iters>
Then publishes a date-stamped snapshot and README-ready markdown table.
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

calc_speedup() {
    local numerator="$1"
    local denominator="$2"
    awk -v n="$numerator" -v d="$denominator" 'BEGIN { if (d == 0.0) printf("0.000"); else printf("%.3f", n / d) }'
}

build_dir="./build"
bench_dir="/tmp/liric_bench"
out_dir="docs/benchmarks"
iters="3"
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

if [[ "$no_run" == "0" ]]; then
    [[ -x "${build_dir}/bench_compat_check" ]] || die "missing executable: ${build_dir}/bench_compat_check"
    [[ -x "${build_dir}/bench_ll" ]] || die "missing executable: ${build_dir}/bench_ll"

    "${build_dir}/bench_compat_check" --timeout "$compat_timeout" --bench-dir "$bench_dir"
    "${build_dir}/bench_ll" --iters "$iters" --bench-dir "$bench_dir"
fi

compat_check_jsonl="$(to_abs_path "${bench_dir}/compat_check.jsonl")"
bench_ll_jsonl="$(to_abs_path "${bench_dir}/bench_ll.jsonl")"
bench_ll_summary="$(to_abs_path "${bench_dir}/bench_ll_summary.json")"

[[ -s "$compat_check_jsonl" ]] || die "missing artifact: ${compat_check_jsonl}"
[[ -s "$bench_ll_jsonl" ]] || die "missing artifact: ${bench_ll_jsonl}"
[[ -s "$bench_ll_summary" ]] || die "missing artifact: ${bench_ll_summary}"

status="$(json_string_field "$bench_ll_summary" "status")"
[[ "$status" == "OK" ]] || die "bench_ll_summary status must be OK (got '${status}')"

tests="$(json_int_field "$bench_ll_summary" "tests")"
summary_iters="$(json_int_field "$bench_ll_summary" "iters")"
liric_parse_ms="$(json_number_field "$bench_ll_summary" "liric_parse_median_ms")"
liric_compile_ms="$(json_number_field "$bench_ll_summary" "liric_compile_median_ms")"
lli_parse_ms="$(json_number_field "$bench_ll_summary" "lli_parse_median_ms")"
lli_jit_ms="$(json_number_field "$bench_ll_summary" "lli_jit_median_ms")"

liric_total_ms="$(awk -v a="$liric_parse_ms" -v b="$liric_compile_ms" 'BEGIN { printf("%.6f", a + b) }')"
llvm_total_ms="$(awk -v a="$lli_parse_ms" -v b="$lli_jit_ms" 'BEGIN { printf("%.6f", a + b) }')"
parse_speedup="$(calc_speedup "$lli_parse_ms" "$liric_parse_ms")"
compile_speedup="$(calc_speedup "$lli_jit_ms" "$liric_compile_ms")"
total_speedup="$(calc_speedup "$llvm_total_ms" "$liric_total_ms")"

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
  "dataset": {
    "tests": ${tests},
    "iters": ${summary_iters}
  },
  "published_table": {
    "parse": {
      "liric_ms": ${liric_parse_ms},
      "llvm_orc_ms": ${lli_parse_ms},
      "speedup": ${parse_speedup}
    },
    "compile": {
      "liric_ms": ${liric_compile_ms},
      "llvm_orc_ms": ${lli_jit_ms},
      "speedup": ${compile_speedup}
    },
    "total_parse_compile": {
      "liric_ms": ${liric_total_ms},
      "llvm_orc_ms": ${llvm_total_ms},
      "speedup": ${total_speedup}
    }
  },
  "artifacts": {
    "compat_check_jsonl": "$(json_escape "$compat_check_jsonl")",
    "bench_ll_jsonl": "$(json_escape "$bench_ll_jsonl")",
    "bench_ll_summary_json": "$(json_escape "$bench_ll_summary")",
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
Dataset: ${tests} tests from tests/ll, ${summary_iters} iterations

Artifacts:
- ${compat_check_jsonl}
- ${bench_ll_jsonl}
- ${bench_ll_summary}
- ${snapshot_path}

| Phase | liric (ms) | LLVM ORC (ms) | Speedup |
|-------|-----------:|--------------:|--------:|
| Parse | $(fmt_fixed "$liric_parse_ms" 3) | $(fmt_fixed "$lli_parse_ms" 3) | $(fmt_fixed "$parse_speedup" 1)x |
| Compile | $(fmt_fixed "$liric_compile_ms" 3) | $(fmt_fixed "$lli_jit_ms" 3) | $(fmt_fixed "$compile_speedup" 1)x |
| Total (parse+compile) | $(fmt_fixed "$liric_total_ms" 3) | $(fmt_fixed "$llvm_total_ms" 3) | $(fmt_fixed "$total_speedup" 1)x |
EOF_MD

echo "Published snapshot: ${snapshot_path}"
echo "Published table:    ${table_path}"
