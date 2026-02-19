#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage: nightly_mass.sh [options]
  --lfortran-root PATH      lfortran repo root (required unless --compat-jsonl is set)
  --lfortran-bin PATH       lfortran binary path (required unless --compat-jsonl is set)
  --probe-runner PATH       liric_probe_runner path (required unless --compat-jsonl is set)
  --runtime-lib PATH        liblfortran_runtime path for lli (auto-detected by default)
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
unsupported_bucket_map_path="${script_dir}/unsupported_bucket_map.json"

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
[[ -f "$unsupported_bucket_map_path" ]] || die "unsupported bucket map not found: $unsupported_bucket_map_path"

manifest_path="${output_root}/manifest_tests_toml.jsonl"
selection_path="${output_root}/selection_decisions.jsonl"
results_path="${output_root}/results.jsonl"
summary_json_path="${output_root}/summary.json"
summary_md_path="${output_root}/summary.md"
failures_path="${output_root}/failures.csv"
unsupported_coverage_path="${output_root}/unsupported_bucket_coverage.md"

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
def str_lc($v): ($v // "" | tostring | ascii_downcase);
def has_any($s; $patterns): reduce $patterns[] as $p (false; . or ($s | contains($p)));
def classify_liric_fail:
  (str_lc(.error)) as $err |
  if has_any($err; ["unresolved symbol", "function not found", "unsupported signature", "entrypoint", "dlsym"])
  then "unsupported_abi"
  else "unsupported_feature"
  end;
def mismatch_symptom:
  if (.llvm_rc != .liric_rc) then "rc-mismatch"
  else "wrong-stdout"
  end;
def feature_family:
  (str_lc(.options)) as $opts |
  (str_lc(.error)) as $err |
  if ($opts | contains("openmp")) or ($err | contains("openmp")) then "openmp"
  elif has_any($err; ["intrinsic", "memset", "memcpy", "memmove", "libm", "math"]) then "intrinsics"
  elif ($err | contains("complex")) then "complex"
  else "general"
  end;
def classification:
  if (.llvm_ok | not) then "lfortran_emit_fail"
  elif (.liric_ok | not) then classify_liric_fail
  elif (.liric_match | not) then "mismatch"
  else "pass"
  end;
def stage($c):
  if $c == "lfortran_emit_fail" then "codegen"
  elif $c == "unsupported_abi" then "jit-link"
  elif $c == "unsupported_feature" then "runtime"
  elif $c == "mismatch" then "output-format"
  else "pass"
  end;
def taxonomy($c):
  if $c == "pass" then null
  elif $c == "lfortran_emit_fail" then "codegen|compiler-error|general"
  elif $c == "unsupported_abi" then "jit-link|unresolved-symbol|runtime-api"
  elif $c == "unsupported_feature" then ("runtime|unsupported-feature|" + feature_family)
  elif $c == "mismatch" then ("output-format|" + mismatch_symptom + "|general")
  else "runtime|unknown|general"
  end;
. as $row |
(classification) as $classification |
{
  "case_id": $row.name,
  "filename": $row.name,
  "source_path": $row.source,
  "corpus": "integration_cmake",
  "emit_attempted": true,
  "emit_ok": $row.llvm_ok,
  "parse_attempted": $row.llvm_ok,
  "parse_ok": $row.liric_ok,
  "jit_attempted": $row.llvm_ok,
  "jit_ok": $row.liric_ok,
  "run_requested": true,
  "differential_attempted": ($row.llvm_ok and $row.liric_ok),
  "differential_ok": ($row.llvm_ok and $row.liric_ok),
  "differential_match": (if ($row.llvm_ok and $row.liric_ok) then $row.liric_match else null end),
  "differential_rc_match": (if ($row.llvm_ok and $row.liric_ok) then ($row.llvm_rc == $row.liric_rc) else null end),
  "differential_stdout_match": (if ($row.llvm_ok and $row.liric_ok) then $row.liric_match else null end),
  "differential_stderr_match": (if ($row.llvm_ok and $row.liric_ok) then true else null end),
  "stage": stage($classification),
  "taxonomy_node": taxonomy($classification),
  "raw_error": ($row.error // ""),
  "reason":
    (if $classification == "lfortran_emit_fail" then "lfortran emission/native execution failed"
     elif $classification == "unsupported_abi" then
         (if (($row.error // "") != "") then ("liric jit ABI/link failure: " + ($row.error // ""))
          else "liric jit ABI/link failure"
          end)
     elif $classification == "unsupported_feature" then
         (if (($row.error // "") != "") then ("liric unsupported feature/runtime failure: " + ($row.error // ""))
          else "liric unsupported feature/runtime failure"
          end)
     elif $classification == "mismatch" then "differential mismatch"
     else ""
     end),
  "classification": $classification
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
    --slurpfile bucket_map "$unsupported_bucket_map_path" \
'
def counts($arr):
  reduce $arr[] as $r ({}; .[$r.classification] = ((.[$r.classification] // 0) + 1));
def taxonomy_counts($arr; $classes):
  reduce $arr[] as $r (
    {};
    if ($r.taxonomy_node == null) then .
    elif ($classes == null or ($classes | index($r.classification))) then
      .[$r.taxonomy_node] = ((.[$r.taxonomy_node] // 0) + 1)
    else .
    end
  );
def sorted_entries($obj):
  ($obj // {} | to_entries | sort_by(-.value, .key));
def unsupported_total($arr):
  [ $arr[] | select(.classification == "unsupported_feature" or .classification == "unsupported_abi") ] | length;
def bucket_coverage($counts; $map):
  [ sorted_entries($counts)[] as $entry
    | ($map[$entry.key] // {}) as $meta
    | ($meta.issues // []) as $issues
    | {
        "taxonomy_node": $entry.key,
        "count": $entry.value,
        "mapped": (($issues | length) > 0 or (($meta.rationale // "") != "")),
        "issues": ($issues | map("#" + tostring)),
        "rationale": ($meta.rationale // "")
      }
  ];
($current) as $rows |
($baseline) as $baseline_rows |
($bucket_map[0] // {}) as $bucket_map_obj |
($baseline_rows | map({key: .case_id, value: .classification}) | from_entries) as $baseline_map |
($rows | counts(.)) as $classification_counts |
($classification_counts.lfortran_emit_fail // 0) as $lfortran_emit_fail_count |
($classification_counts.unsupported_abi // 0) as $unsupported_abi_count |
($classification_counts.unsupported_feature // 0) as $unsupported_feature_count |
($rows | taxonomy_counts(.; null)) as $all_taxonomy_counts |
($rows | taxonomy_counts(.; ["mismatch"])) as $mismatch_taxonomy_counts |
($rows | taxonomy_counts(.; ["unsupported_feature", "unsupported_abi"])) as $unsupported_taxonomy_counts |
($baseline_rows | taxonomy_counts(.; ["unsupported_feature", "unsupported_abi"])) as $baseline_unsupported_taxonomy_counts |
($mismatch_taxonomy_counts | bucket_coverage(.; $bucket_map_obj)) as $mismatch_bucket_issue_coverage |
($rows | unsupported_total(.)) as $unsupported_total_current |
($baseline_rows | unsupported_total(.)) as $unsupported_total_baseline |
($unsupported_total_current - $unsupported_total_baseline) as $unsupported_total_delta |
(
  [ (($unsupported_taxonomy_counts + $baseline_unsupported_taxonomy_counts) | keys_unsorted[]) ]
  | unique
  | sort
  | map(
      . as $k |
      {
        "taxonomy_node": $k,
        "current": ($unsupported_taxonomy_counts[$k] // 0),
        "baseline": ($baseline_unsupported_taxonomy_counts[$k] // 0),
        "delta": (($unsupported_taxonomy_counts[$k] // 0) - ($baseline_unsupported_taxonomy_counts[$k] // 0)),
        "trend":
          (if (($unsupported_taxonomy_counts[$k] // 0) < ($baseline_unsupported_taxonomy_counts[$k] // 0)) then "improving"
           elif (($unsupported_taxonomy_counts[$k] // 0) > ($baseline_unsupported_taxonomy_counts[$k] // 0)) then "regressing"
           else "stable"
           end)
      }
    )
) as $unsupported_taxonomy_delta |
($unsupported_taxonomy_counts | bucket_coverage(.; $bucket_map_obj)) as $unsupported_bucket_issue_coverage |
(
  [ $mismatch_bucket_issue_coverage[]
    | select(.mapped | not)
    | .taxonomy_node
  ]
) as $mismatch_bucket_unmapped |
(
  [ $unsupported_bucket_issue_coverage[]
    | select(.mapped | not)
    | .taxonomy_node
  ]
) as $unsupported_bucket_unmapped |
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
(
  [
    (if $lfortran_emit_fail_count > 0 then "lfortran_emit_fail" else empty end),
    (if $mismatch_count > 0 then "mismatch" else empty end),
    (if (($regressed_case_ids | length) > 0) then "new_supported_regressions" else empty end),
    (if $diff_missing_attempts > 0 then "differential_missing_attempts" else empty end)
  ]
) as $gate_fail_reasons |
($unsupported_abi_count + $unsupported_feature_count + $mismatch_count) as $liric_compat_failure_count |
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
  "classification_counts": $classification_counts,
  "taxonomy_counts": $all_taxonomy_counts,
  "mismatch_taxonomy_counts": $mismatch_taxonomy_counts,
  "mismatch_bucket_issue_coverage": $mismatch_bucket_issue_coverage,
  "mismatch_bucket_unmapped": $mismatch_bucket_unmapped,
  "unsupported_taxonomy_counts": $unsupported_taxonomy_counts,
  "unsupported_bucket_issue_coverage": $unsupported_bucket_issue_coverage,
  "unsupported_bucket_unmapped": $unsupported_bucket_unmapped,
  "unsupported_total_baseline": $unsupported_total_baseline,
  "unsupported_total_current": $unsupported_total_current,
  "unsupported_total_delta": $unsupported_total_delta,
  "unsupported_total_trend":
    (if $unsupported_total_delta < 0 then "improving"
     elif $unsupported_total_delta > 0 then "regressing"
     else "stable"
     end),
  "unsupported_taxonomy_delta": $unsupported_taxonomy_delta,
  "mismatch_count": $mismatch_count,
  "lfortran_emit_fail_count": $lfortran_emit_fail_count,
  "liric_compat_failure_count": $liric_compat_failure_count,
  "new_supported_regressions": ($regressed_case_ids | length),
  "regressed_case_ids": $regressed_case_ids,
  "gate_fail_reasons": $gate_fail_reasons,
  "gate_fail": (($gate_fail_reasons | length) > 0)
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
      "- LFortran emit blockers: \(.lfortran_emit_fail_count)\n" +
      "- Liric compatibility failures: \(.liric_compat_failure_count)\n" +
      "- Unsupported total baseline: \(.unsupported_total_baseline)\n" +
      "- Unsupported total current: \(.unsupported_total_current)\n" +
      "- Unsupported total delta: \(.unsupported_total_delta)\n" +
      "- Unsupported total trend: \(.unsupported_total_trend)\n" +
      "- New supported regressions: \(.new_supported_regressions)\n" +
      "- Gate fail reasons: \((.gate_fail_reasons // []) | if length == 0 then "none" else join(", ") end)\n" +
      "- Gate fail: \(.gate_fail)"
    ' "$summary_json_path"
    echo
    echo "## Classification Counts"
    echo
    jq -r '
      (.classification_counts // {}) as $counts |
      if ($counts | length) == 0 then "- (none)"
      else
        $counts
        | to_entries
        | sort_by(.key)
        | .[]
        | "- \(.key): \(.value)"
      end
    ' "$summary_json_path"
    echo
    echo "## Taxonomy Counts (Mismatch)"
    echo
    jq -r '
      (.mismatch_taxonomy_counts // {}) as $counts |
      if ($counts | length) == 0 then "- (none)"
      else
        $counts
        | to_entries
        | sort_by(-.value, .key)
        | .[]
        | "- \(.key): \(.value)"
      end
    ' "$summary_json_path"
    echo
    echo "## Mismatch Bucket Coverage"
    echo
    jq -r '
      (.mismatch_bucket_issue_coverage // []) as $rows |
      if ($rows | length) == 0 then "- (none)"
      else
        $rows[]
        | "- \(.taxonomy_node): \(.count) -> " +
          (if ((.issues // []) | length) > 0 then (.issues | join(", "))
           elif ((.rationale // "") != "") then "deferred"
           else "unmapped"
           end) +
          (if ((.rationale // "") != "") then " (\(.rationale))"
           else ""
           end)
      end
    ' "$summary_json_path"
    echo
    echo "## Mismatch Bucket Mapping Gaps"
    echo
    jq -r '
      (.mismatch_bucket_unmapped // []) as $rows |
      if ($rows | length) == 0 then "- none"
      else
        $rows[]
        | "- " + .
      end
    ' "$summary_json_path"
    echo
    echo "## Taxonomy Counts (Unsupported)"
    echo
    jq -r '
      (.unsupported_taxonomy_counts // {}) as $counts |
      if ($counts | length) == 0 then "- (none)"
      else
        $counts
        | to_entries
        | sort_by(-.value, .key)
        | .[]
        | "- \(.key): \(.value)"
      end
    ' "$summary_json_path"
    echo
    echo "## Unsupported Bucket Coverage"
    echo
    jq -r '
      (.unsupported_bucket_issue_coverage // []) as $rows |
      if ($rows | length) == 0 then "- (none)"
      else
        $rows[]
        | "- \(.taxonomy_node): \(.count) -> " +
          (if ((.issues // []) | length) > 0 then (.issues | join(", "))
           elif ((.rationale // "") != "") then "deferred"
           else "unmapped"
           end) +
          (if ((.rationale // "") != "") then " (\(.rationale))"
           else ""
           end)
      end
    ' "$summary_json_path"
    echo
    echo "## Unsupported Bucket Mapping Gaps"
    echo
    jq -r '
      (.unsupported_bucket_unmapped // []) as $rows |
      if ($rows | length) == 0 then "- none"
      else
        $rows[]
        | "- " + .
      end
    ' "$summary_json_path"
} > "$summary_md_path"

jq -r '
  "# Unsupported Bucket Coverage\n\n" +
  "| Bucket | Count | Mapping | Rationale |\n" +
  "|---|---:|---|---|\n" +
  (
    (.unsupported_bucket_issue_coverage // []) as $rows |
    if ($rows | length) == 0 then
      "| (none) | 0 | n/a | n/a |"
    else
      (
        $rows
        | map(
            "| `\(.taxonomy_node)` | \(.count) | " +
            (if ((.issues // []) | length) > 0 then (.issues | join(", "))
             elif ((.rationale // "") != "") then "deferred"
             else "unmapped"
             end) +
            " | " +
            (((.rationale // "") | gsub("\\|"; "\\\\|"))) +
            " |"
          )
        | join("\n")
      )
    end
  ) +
  "\n\n## Unsupported Trend\n\n" +
  "- baseline: \(.unsupported_total_baseline // 0)\n" +
  "- current: \(.unsupported_total_current // 0)\n" +
  "- delta: \(.unsupported_total_delta // 0)\n" +
  "- trend: \(.unsupported_total_trend // "stable")\n" +
  "\n## Unmapped Buckets\n\n" +
  (
    (.unsupported_bucket_unmapped // []) as $rows |
    if ($rows | length) == 0 then "- none"
    else ($rows | map("- " + .) | join("\n"))
    end
  ) +
  "\n"
' "$summary_json_path" > "$unsupported_coverage_path"

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

gate_reasons="$(jq -r '(.gate_fail_reasons // []) | if length == 0 then "none" else join(", ") end' "$summary_json_path")"
if [[ "$(jq -r '.gate_fail' "$summary_json_path")" == "true" ]]; then
    echo "gate: fail (${gate_reasons})"
    exit 1
fi

echo "gate: pass (no lfortran emit blockers, mismatches, missing attempts, or new supported regressions)"
