#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage: render_fail_tasklist.sh --results-jsonl PATH --summary-json PATH --out PATH

Render a markdown checklist for every non-pass case in nightly_mass results.
EOF
}

die() {
    echo "render_fail_tasklist: $*" >&2
    exit 1
}

results_jsonl=""
summary_json=""
out_path=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --results-jsonl)
            [[ $# -ge 2 ]] || die "missing value for $1"
            results_jsonl="$2"
            shift 2
            ;;
        --summary-json)
            [[ $# -ge 2 ]] || die "missing value for $1"
            summary_json="$2"
            shift 2
            ;;
        --out)
            [[ $# -ge 2 ]] || die "missing value for $1"
            out_path="$2"
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

[[ -f "$results_jsonl" ]] || die "results file not found: $results_jsonl"
[[ -f "$summary_json" ]] || die "summary file not found: $summary_json"
[[ -n "$out_path" ]] || die "--out is required"

mkdir -p "$(dirname "$out_path")"

{
    echo "# LFortran + Liric Failure Task List"
    echo
    echo "Generated: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo
    echo "Source artifacts:"
    echo "- summary: \`$summary_json\`"
    echo "- results: \`$results_jsonl\`"
    echo
    echo "## Snapshot"
    echo
    jq -r '
      "- total: \(.manifest_total)\n" +
      "- pass: \(.classification_counts.pass // 0)\n" +
      "- mismatch: \(.classification_counts.mismatch // 0)\n" +
      "- lfortran_emit_fail: \(.classification_counts.lfortran_emit_fail // 0)\n" +
      "- unsupported_abi: \(.classification_counts.unsupported_abi // 0)\n" +
      "- unsupported_feature: \(.classification_counts.unsupported_feature // 0)\n" +
      "- compile_blockers: \(.lfortran_emit_fail_count // (.classification_counts.lfortran_emit_fail // 0))\n" +
      "- liric_compat_failures: \(.liric_compat_failure_count // ((.classification_counts.unsupported_abi // 0) + (.classification_counts.unsupported_feature // 0) + (.classification_counts.mismatch // 0)))\n" +
      "- gate_fail: \(.gate_fail)"
    ' "$summary_json"
    echo

    for cls in mismatch lfortran_emit_fail unsupported_abi unsupported_feature; do
        count="$(jq -r --arg cls "$cls" '.classification_counts[$cls] // 0' "$summary_json")"
        echo "## ${cls} (${count})"
        echo
        jq -r --arg cls "$cls" '
          select(.classification == $cls)
          | "- [ ] \(.case_id) (`\(.taxonomy_node // "n/a")`) - \(.reason)"
        ' "$results_jsonl"
        echo
    done
} > "$out_path"

echo "rendered: $out_path"
