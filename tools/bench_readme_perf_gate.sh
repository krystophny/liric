#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/bench_shell_common.sh
source "${script_dir}/bench_shell_common.sh"
BENCH_SCRIPT_NAME="bench_readme_perf_gate"

usage() {
    cat <<'USAGE'
usage: bench_readme_perf_gate.sh [options]
  --readme PATH      README path (default: ./README.md)
  --snapshot PATH    benchmark snapshot JSON (default: ./docs/benchmarks/readme_perf_snapshot.json)
  --table PATH       benchmark table markdown (default: ./docs/benchmarks/readme_perf_table.md)
  -h, --help         show this help

Gate conditions (must all be true):
  - README references published benchmark artifacts and regeneration command.
  - Snapshot JSON uses corpus_100 dataset and canonical corpus_canonical track.
  - Canonical track completion equals expected_tests (full real-corpus publish).
  - Published table includes canonical-track row and real-corpus metadata.
  - Obsolete smoke/legacy fields are absent.
USAGE
}

readme="./README.md"
snapshot="./docs/benchmarks/readme_perf_snapshot.json"
table="./docs/benchmarks/readme_perf_table.md"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --readme)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            readme="$2"
            shift 2
            ;;
        --snapshot)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            snapshot="$2"
            shift 2
            ;;
        --table)
            [[ $# -ge 2 ]] || bench_die "missing value for $1"
            table="$2"
            shift 2
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

bench_require_nonempty_file "$readme"
bench_require_nonempty_file "$snapshot"
bench_require_nonempty_file "$table"

generated_at="$(bench_json_string_field "$snapshot" "generated_at_utc")"
benchmark_commit="$(bench_json_string_field "$snapshot" "benchmark_commit")"
status="$(bench_json_string_field "$snapshot" "status")"
dataset_name="$(bench_json_string_field "$snapshot" "dataset_name")"
canonical_track="$(bench_json_string_field "$snapshot" "canonical_track")"
expected_tests="$(bench_json_int_field "$snapshot" "expected_tests")"
attempted_tests="$(bench_json_int_field "$snapshot" "attempted_tests")"
iters="$(bench_json_int_field "$snapshot" "iters")"
completed_tests="$(bench_json_int_field "$snapshot" "completed_tests")"
bench_corpus_compare_summary_json="$(bench_json_string_field "$snapshot" "bench_corpus_compare_summary_json")"
bench_corpus_compare_jsonl="$(bench_json_string_field "$snapshot" "bench_corpus_compare_jsonl")"
published_snapshot_json="$(bench_json_string_field "$snapshot" "published_snapshot_json")"
published_table_md="$(bench_json_string_field "$snapshot" "published_table_md")"

[[ "$generated_at" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T ]] || bench_die "generated_at_utc is not ISO-8601-like: ${generated_at}"
[[ "$generated_at" =~ Z$ ]] || bench_die "generated_at_utc must be UTC (Z suffix): ${generated_at}"
[[ "$benchmark_commit" =~ ^[0-9a-fA-F]{7,40}$ ]] || bench_die "benchmark_commit is not a git hash: ${benchmark_commit}"
[[ "$dataset_name" == "corpus_100" ]] || bench_die "dataset_name must be corpus_100 (got ${dataset_name})"
[[ "$canonical_track" == "corpus_canonical" ]] || bench_die "canonical_track must be corpus_canonical (got ${canonical_track})"
[[ "$expected_tests" -eq 100 ]] || bench_die "expected_tests must be 100 (got ${expected_tests})"
[[ "$attempted_tests" -gt 0 ]] || bench_die "attempted_tests must be > 0"
[[ "$iters" -gt 0 ]] || bench_die "iters must be > 0"
[[ "$completed_tests" -eq "$attempted_tests" ]] || bench_die "canonical track must complete all attempted tests (${completed_tests}/${attempted_tests})"
min_attempted=$(( expected_tests * 90 / 100 ))
[[ "$attempted_tests" -ge "$min_attempted" ]] || bench_die "attempted tests must be >= 90% of expected (${attempted_tests}/${expected_tests})"
[[ "$status" == "OK" ]] || bench_die "status must be OK for publishable README snapshot (got ${status})"

[[ -n "$bench_corpus_compare_summary_json" ]] || bench_die "bench_corpus_compare_summary_json path is empty"
[[ -n "$bench_corpus_compare_jsonl" ]] || bench_die "bench_corpus_compare_jsonl path is empty"
[[ -n "$published_snapshot_json" ]] || bench_die "published_snapshot_json path is empty"
[[ -n "$published_table_md" ]] || bench_die "published_table_md path is empty"

[[ "$published_snapshot_json" == */readme_perf_snapshot.json ]] ||
    bench_die "published_snapshot_json must end with readme_perf_snapshot.json"
[[ "$published_table_md" == */readme_perf_table.md ]] ||
    bench_die "published_table_md must end with readme_perf_table.md"

bench_require_pattern "$table" '^Generated:[[:space:]]+[0-9]{4}-[0-9]{2}-[0-9]{2}T' "table missing Generated line"
bench_require_pattern "$table" '^Benchmark commit:[[:space:]]+[0-9a-fA-F]{7,40}' "table missing benchmark commit line"
bench_require_pattern "$table" '^Dataset:[[:space:]]+corpus_100' "table missing corpus_100 dataset line"
bench_require_pattern "$table" '^Canonical track:[[:space:]]+corpus_canonical' "table missing canonical track line"
bench_require_pattern "$table" '^Artifacts:' "table missing Artifacts section"
if ! grep -Eq '^\| Track \| Completed \| liric parse \(ms\) \| liric compile\+lookup \(ms\) \| liric total materialized \(ms\) \| LLVM parse \(ms\) \| LLVM add\+lookup \(ms\) \| LLVM total materialized \(ms\) \| (Speedup non-parse \(median\) \| Speedup non-parse \(aggregate\) \| )?Speedup total \(median\) \| Speedup total \(aggregate\) \|' "$table"; then
    bench_die "table missing metric header (${table})"
fi
bench_require_pattern "$table" '^\| corpus_canonical \(canonical\) \|' "table missing canonical track row"

bench_require_pattern "$readme" 'docs/benchmarks/readme_perf_snapshot\.json' "README missing snapshot artifact path"
bench_require_pattern "$readme" 'docs/benchmarks/readme_perf_table\.md' "README missing table artifact path"
bench_require_pattern "$readme" '\./tools/bench_readme_perf_snapshot\.sh' "README missing snapshot regeneration command"
bench_require_pattern "$readme" '\./build/bench_corpus_compare --iters' "README missing corpus comparator benchmark command"

bench_forbid_pattern "$snapshot" '"published_table"[[:space:]]*:' "snapshot contains obsolete published_table object"
bench_forbid_pattern "$snapshot" '"bench_ll_summary_json"[[:space:]]*:' "snapshot contains obsolete bench_ll artifact reference"
bench_forbid_pattern "$snapshot" '"bench_corpus_compare_core_jsonl"[[:space:]]*:' "snapshot contains obsolete dual-track artifact reference"
bench_forbid_pattern "$snapshot" '"bench_corpus_compare_runtime_equalized_bc_jsonl"[[:space:]]*:' "snapshot contains obsolete dual-track artifact reference"
bench_forbid_pattern "$snapshot" 'readme_smoke' "snapshot contains smoke artifact path"
bench_forbid_pattern "$table" 'readme_smoke' "table contains smoke artifact path"
bench_forbid_pattern "$table" 'Total \(parse\+compile\)' "table contains obsolete legacy metric wording"

echo "bench_readme_perf_gate: PASSED"
echo "  generated_at_utc=${generated_at}"
echo "  benchmark_commit=${benchmark_commit}"
echo "  dataset_name=${dataset_name}"
echo "  expected_tests=${expected_tests}"
echo "  attempted_tests=${attempted_tests}"
echo "  completed_tests=${completed_tests}"
echo "  snapshot=${snapshot}"
echo "  table=${table}"
echo "  readme=${readme}"
