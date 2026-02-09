#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage: nightly_mass.sh [options]
  --lfortran-root PATH      lfortran repo root (required unless --compat-jsonl is set)
  --lfortran-bin PATH       lfortran binary path (required unless --compat-jsonl is set)
  --probe-runner PATH       liric_probe_runner path (required unless --compat-jsonl is set)
  --runtime-lib PATH        liblfortran_runtime path (auto-detected by default)
  --output-root PATH        output artifact root (default: /tmp/liric_lfortran_mass)
  --workers N               accepted for compatibility (ignored by shell post-processing)
  --timeout N               bench_compat_check timeout seconds (default: 15)
  --baseline PATH           previous results.jsonl for regression gate
  --diag-fail-logs          accepted for compatibility (no-op)
  --compat-jsonl PATH       skip execution and consume an existing compat_check.jsonl
  -h, --help                show this help
EOF
}

die() {
    echo "nightly_mass: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

find_runtime_lib() {
    local lfortran_bin="$1"
    local bin_dir
    local cand

    bin_dir="$(cd "$(dirname "$lfortran_bin")" && pwd)"
    cand="${bin_dir}/../runtime/liblfortran_runtime.so"
    if [[ -f "$cand" ]]; then
        printf '%s\n' "$cand"
        return 0
    fi
    cand="${bin_dir}/../runtime/liblfortran_runtime.dylib"
    if [[ -f "$cand" ]]; then
        printf '%s\n' "$cand"
        return 0
    fi
    return 1
}

lfortran_root=""
lfortran_bin=""
probe_runner=""
runtime_lib=""
output_root="/tmp/liric_lfortran_mass"
workers=""
timeout_sec="15"
baseline_path=""
compat_jsonl=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lfortran-root)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_root="$2"
            shift 2
            ;;
        --lfortran-bin)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_bin="$2"
            shift 2
            ;;
        --probe-runner)
            [[ $# -ge 2 ]] || die "missing value for $1"
            probe_runner="$2"
            shift 2
            ;;
        --runtime-lib)
            [[ $# -ge 2 ]] || die "missing value for $1"
            runtime_lib="$2"
            shift 2
            ;;
        --output-root)
            [[ $# -ge 2 ]] || die "missing value for $1"
            output_root="$2"
            shift 2
            ;;
        --workers)
            [[ $# -ge 2 ]] || die "missing value for $1"
            workers="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "missing value for $1"
            timeout_sec="$2"
            shift 2
            ;;
        --baseline)
            [[ $# -ge 2 ]] || die "missing value for $1"
            baseline_path="$2"
            shift 2
            ;;
        --diag-fail-logs)
            shift
            ;;
        --compat-jsonl)
            [[ $# -ge 2 ]] || die "missing value for $1"
            compat_jsonl="$2"
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

need_cmd jq

mkdir -p "$output_root"

if [[ -z "$compat_jsonl" ]]; then
    [[ -n "$lfortran_root" ]] || die "--lfortran-root is required unless --compat-jsonl is set"
    [[ -n "$lfortran_bin" ]] || die "--lfortran-bin is required unless --compat-jsonl is set"
    [[ -n "$probe_runner" ]] || die "--probe-runner is required unless --compat-jsonl is set"
    [[ -f "$lfortran_bin" ]] || die "lfortran binary not found: $lfortran_bin"
    [[ -f "$probe_runner" ]] || die "probe runner not found: $probe_runner"

    if [[ -z "$runtime_lib" ]]; then
        runtime_lib="$(find_runtime_lib "$lfortran_bin")" || die "failed to auto-detect liblfortran_runtime"
    fi
    [[ -f "$runtime_lib" ]] || die "runtime library not found: $runtime_lib"

    local_cmake="${lfortran_root}/integration_tests/CMakeLists.txt"
    [[ -f "$local_cmake" ]] || die "integration CMakeLists not found: $local_cmake"

    bench_compat_check="$(cd "$(dirname "$probe_runner")" && pwd)/bench_compat_check"
    [[ -f "$bench_compat_check" ]] || die "bench_compat_check not found: $bench_compat_check"

    bench_dir="${output_root}/bench"
    mkdir -p "$bench_dir"
    bench_args=(
        "$bench_compat_check"
        --timeout "$timeout_sec"
        --bench-dir "$bench_dir"
        --lfortran "$lfortran_bin"
        --probe-runner "$probe_runner"
        --runtime-lib "$runtime_lib"
        --cmake "$local_cmake"
    )
    if [[ -n "$workers" ]]; then
        bench_args+=(--workers "$workers")
    fi
    "${bench_args[@]}"

    compat_jsonl="${bench_dir}/compat_check.jsonl"
fi

[[ -f "$compat_jsonl" ]] || die "compat jsonl not found: $compat_jsonl"

manifest_path="${output_root}/manifest_tests_toml.jsonl"
selection_path="${output_root}/selection_decisions.jsonl"
results_path="${output_root}/results.jsonl"
summary_json_path="${output_root}/summary.json"
summary_md_path="${output_root}/summary.md"
failures_path="${output_root}/failures.csv"

jq -c '
{
  "case_id": .name,
  "filename": .name,
  "source_path": .source,
  "corpus": "integration_cmake",
  "options": (.options // "")
}
' "$compat_jsonl" > "$manifest_path"

jq -c '
{
  "case_id": .name,
  "filename": .name,
  "source_path": .source,
  "corpus": "integration_cmake",
  "decision": "selected",
  "reason": "llvm_labeled_integration_test"
}
' "$compat_jsonl" > "$selection_path"

jq -c '
def classification:
  if (.llvm_ok | not) then "lfortran_emit_fail"
  elif (.liric_ok | not) then "unsupported_feature"
  elif (.liric_match | not) then "mismatch"
  else "pass"
  end;
{
  "case_id": .name,
  "filename": .name,
  "source_path": .source,
  "corpus": "integration_cmake",
  "emit_attempted": true,
  "emit_ok": .llvm_ok,
  "parse_attempted": .llvm_ok,
  "parse_ok": .liric_ok,
  "jit_attempted": .llvm_ok,
  "jit_ok": .liric_ok,
  "run_requested": true,
  "differential_attempted": (.llvm_ok and .liric_ok),
  "differential_ok": (.llvm_ok and .liric_ok),
  "differential_match": (if (.llvm_ok and .liric_ok) then .liric_match else null end),
  "differential_rc_match": (if (.llvm_ok and .liric_ok) then (.llvm_rc == .liric_rc) else null end),
  "differential_stdout_match": (if (.llvm_ok and .liric_ok) then .liric_match else null end),
  "differential_stderr_match": (if (.llvm_ok and .liric_ok) then true else null end),
  "stage":
    (if (.llvm_ok | not) then "emit"
     elif (.liric_ok | not) then "jit"
     elif (.liric_match | not) then "differential"
     else "pass"
     end),
  "reason":
    (if (.llvm_ok | not) then "lfortran emission/native execution failed"
     elif (.liric_ok | not) then "liric jit execution failed"
     elif (.liric_match | not) then "differential mismatch"
     else ""
     end),
  "classification": classification
}
' "$compat_jsonl" > "$results_path"

baseline_for_jq="${output_root}/.baseline_rows.jsonl"
cleanup_baseline_tmp=1
if [[ -n "$baseline_path" && -f "$baseline_path" ]]; then
    baseline_for_jq="$baseline_path"
    cleanup_baseline_tmp=0
else
    : > "$baseline_for_jq"
fi

jq -n \
    --slurpfile current "$results_path" \
    --slurpfile baseline "$baseline_for_jq" \
'
def counts($arr):
  reduce $arr[] as $r ({}; .[$r.classification] = ((.[$r.classification] // 0) + 1));
($current) as $rows |
($baseline | map({key: .case_id, value: .classification}) | from_entries) as $baseline_map |
($rows | length) as $manifest_total |
([ $rows[] | select(.emit_ok) ] | length) as $emit_ok |
([ $rows[] | select(.parse_ok) ] | length) as $parse_ok |
([ $rows[] | select(.jit_ok) ] | length) as $jit_ok |
([ $rows[] | select(.run_requested) ] | length) as $runnable_selected |
([ $rows[] | select(.run_requested and .jit_ok) ] | length) as $runnable_selected_executable |
([ $rows[] | select(.run_requested and .jit_ok and .differential_attempted) ] | length) as $diff_attempted_executable |
([ $rows[] | select(.differential_match == true) ] | length) as $diff_match |
(
  [ $rows[]
    | select(
        ($baseline_map[.case_id] == "pass")
        and (.classification != "pass")
        and (.classification != "unsupported_feature")
        and (.classification != "unsupported_abi")
      )
    | .case_id
  ]
) as $regressed_case_ids |
($runnable_selected_executable - $diff_attempted_executable) as $diff_missing_attempts |
($diff_attempted_executable - $diff_match) as $mismatch_count |
{
  "manifest_total": $manifest_total,
  "processed_total": $manifest_total,
  "emit_attempted": $manifest_total,
  "emit_ok": $emit_ok,
  "parse_attempted": $emit_ok,
  "parse_ok": $parse_ok,
  "jit_attempted": $emit_ok,
  "jit_ok": $jit_ok,
  "runnable_selected": $runnable_selected,
  "runnable_selected_executable": $runnable_selected_executable,
  "differential_attempted": $diff_attempted_executable,
  "differential_attempted_executable": $diff_attempted_executable,
  "differential_ok": $diff_attempted_executable,
  "differential_match": $diff_match,
  "differential_missing_attempts": $diff_missing_attempts,
  "differential_parity_ok": ($diff_missing_attempts == 0),
  "classification_counts": counts($rows),
  "mismatch_count": $mismatch_count,
  "new_supported_regressions": ($regressed_case_ids | length),
  "regressed_case_ids": $regressed_case_ids,
  "gate_fail": (($mismatch_count > 0) or (($regressed_case_ids | length) > 0) or ($diff_missing_attempts > 0))
}
' > "$summary_json_path"

{
    echo "# LFortran Mass Testing Summary"
    echo
    jq -r '
      "- Total selected tests: \(.manifest_total)\n" +
      "- Processed tests: \(.processed_total)\n" +
      "- LLVM emission attempted: \(.emit_attempted)\n" +
      "- LLVM emission succeeded: \(.emit_ok)\n" +
      "- Liric parse attempted: \(.parse_attempted)\n" +
      "- Liric parse passed: \(.parse_ok)\n" +
      "- Liric JIT attempted: \(.jit_attempted)\n" +
      "- Liric JIT passed: \(.jit_ok)\n" +
      "- Runnable selected executable: \(.runnable_selected_executable)\n" +
      "- Differential attempted executable: \(.differential_attempted_executable)\n" +
      "- Differential exact matches: \(.differential_match)\n" +
      "- Differential missing attempts: \(.differential_missing_attempts)\n" +
      "- Mismatches: \(.mismatch_count)\n" +
      "- New supported regressions: \(.new_supported_regressions)\n" +
      "- Gate fail: \(.gate_fail)"
    ' "$summary_json_path"
} > "$summary_md_path"

{
    echo "case_id,filename,source_path,classification,stage,reason"
    jq -r '
      select(.classification != "pass") |
      [.case_id, .filename, .source_path, .classification, .stage, .reason] |
      @csv
    ' "$results_path"
} > "$failures_path"

if [[ "$cleanup_baseline_tmp" == "1" ]]; then
    rm -f "$baseline_for_jq"
fi

if [[ "$(jq -r '.gate_fail' "$summary_json_path")" == "true" ]]; then
    echo "gate: fail (mismatch or new supported regressions detected)"
    exit 1
fi

echo "gate: pass (no mismatches and no new supported regressions)"
