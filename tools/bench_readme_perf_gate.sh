#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: bench_readme_perf_gate.sh [options]
  --readme PATH      README path (default: ./README.md)
  --snapshot PATH    benchmark snapshot JSON (default: ./docs/benchmarks/readme_perf_snapshot.json)
  --table PATH       benchmark table markdown (default: ./docs/benchmarks/readme_perf_table.md)
  -h, --help         show this help

Gate conditions (must all be true):
  - README references published benchmark artifacts and regen command.
  - Snapshot JSON contains date, commit, dataset size, and artifact path fields.
  - Published table includes generated date, benchmark commit, artifact refs, and metric table.
USAGE
}

die() {
    echo "bench_readme_perf_gate: $*" >&2
    exit 1
}

require_nonempty_file() {
    local path="$1"
    [[ -s "$path" ]] || die "missing or empty file: ${path}"
}

require_pattern() {
    local path="$1"
    local pattern="$2"
    local msg="$3"
    if ! grep -Eq "$pattern" "$path"; then
        die "$msg (${path})"
    fi
}

json_string_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing string field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*"([^"]*)".*/\1/'
}

json_int_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*[0-9]+" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || die "missing integer field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/'
}

readme="./README.md"
snapshot="./docs/benchmarks/readme_perf_snapshot.json"
table="./docs/benchmarks/readme_perf_table.md"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --readme)
            [[ $# -ge 2 ]] || die "missing value for $1"
            readme="$2"
            shift 2
            ;;
        --snapshot)
            [[ $# -ge 2 ]] || die "missing value for $1"
            snapshot="$2"
            shift 2
            ;;
        --table)
            [[ $# -ge 2 ]] || die "missing value for $1"
            table="$2"
            shift 2
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

require_nonempty_file "$readme"
require_nonempty_file "$snapshot"
require_nonempty_file "$table"

generated_at="$(json_string_field "$snapshot" "generated_at_utc")"
benchmark_commit="$(json_string_field "$snapshot" "benchmark_commit")"
tests="$(json_int_field "$snapshot" "tests")"
iters="$(json_int_field "$snapshot" "iters")"
compat_check_jsonl="$(json_string_field "$snapshot" "compat_check_jsonl")"
bench_ll_jsonl="$(json_string_field "$snapshot" "bench_ll_jsonl")"
bench_ll_summary_json="$(json_string_field "$snapshot" "bench_ll_summary_json")"
published_snapshot_json="$(json_string_field "$snapshot" "published_snapshot_json")"
published_table_md="$(json_string_field "$snapshot" "published_table_md")"

[[ "$generated_at" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T ]] || die "generated_at_utc is not ISO-8601-like: ${generated_at}"
[[ "$generated_at" =~ Z$ ]] || die "generated_at_utc must be UTC (Z suffix): ${generated_at}"
[[ "$benchmark_commit" =~ ^[0-9a-fA-F]{7,40}$ ]] || die "benchmark_commit is not a git hash: ${benchmark_commit}"
[[ "$tests" -gt 0 ]] || die "dataset tests must be > 0"
[[ "$iters" -gt 0 ]] || die "dataset iters must be > 0"

[[ -n "$compat_check_jsonl" ]] || die "compat_check_jsonl path is empty"
[[ -n "$bench_ll_jsonl" ]] || die "bench_ll_jsonl path is empty"
[[ -n "$bench_ll_summary_json" ]] || die "bench_ll_summary_json path is empty"
[[ -n "$published_snapshot_json" ]] || die "published_snapshot_json path is empty"
[[ -n "$published_table_md" ]] || die "published_table_md path is empty"

[[ "$published_snapshot_json" == */readme_perf_snapshot.json ]] ||
    die "published_snapshot_json must end with readme_perf_snapshot.json"
[[ "$published_table_md" == */readme_perf_table.md ]] ||
    die "published_table_md must end with readme_perf_table.md"

require_pattern "$snapshot" '"published_table"[[:space:]]*:[[:space:]]*\{' "snapshot missing published_table object"
require_pattern "$snapshot" '"artifacts"[[:space:]]*:[[:space:]]*\{' "snapshot missing artifacts object"

require_pattern "$table" '^Generated:[[:space:]]+[0-9]{4}-[0-9]{2}-[0-9]{2}T' "table missing Generated line"
require_pattern "$table" '^Benchmark commit:[[:space:]]+[0-9a-fA-F]{7,40}' "table missing benchmark commit line"
require_pattern "$table" '^Artifacts:' "table missing Artifacts section"
require_pattern "$table" '^\| Phase \| liric \(ms\) \| LLVM ORC \(ms\) \| Speedup \|' "table missing metric header"
require_pattern "$table" 'readme_perf_snapshot\.json' "table missing snapshot artifact reference"

require_pattern "$readme" 'docs/benchmarks/readme_perf_snapshot\.json' "README missing snapshot artifact path"
require_pattern "$readme" 'docs/benchmarks/readme_perf_table\.md' "README missing table artifact path"
require_pattern "$readme" '\./tools/bench_readme_perf_snapshot\.sh' "README missing snapshot regeneration command"
require_pattern "$readme" '\./build/bench_exe_matrix --iters' "README missing exe matrix benchmark command"

echo "bench_readme_perf_gate: PASSED"
echo "  generated_at_utc=${generated_at}"
echo "  benchmark_commit=${benchmark_commit}"
echo "  dataset.tests=${tests}"
echo "  dataset.iters=${iters}"
echo "  snapshot=${snapshot}"
echo "  table=${table}"
echo "  readme=${readme}"
