#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: issue_tracker_checklist_sync.sh --issue N [options]

Options:
  --issue N         issue number to validate/sync (required)
  --repo OWNER/REPO GitHub repo override (default: current repo)
  --mode MODE       check or sync (default: check)
  --help            show this help

Behavior:
  - Scans checklist lines like "- [ ] #123 ..." or "- [x] #123 ..."
  - Computes expected checkbox state from each referenced issue's actual state
    (open -> [ ], closed -> [x])
  - In check mode: exits non-zero if any checklist item is stale
  - In sync mode: edits the issue body to match expected checkbox states
USAGE
}

die() {
    echo "issue_tracker_checklist_sync: $*" >&2
    exit 1
}

issue_num=""
repo=""
mode="check"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --issue)
            [[ $# -ge 2 ]] || die "missing value for $1"
            issue_num="$2"
            shift 2
            ;;
        --repo)
            [[ $# -ge 2 ]] || die "missing value for $1"
            repo="$2"
            shift 2
            ;;
        --mode)
            [[ $# -ge 2 ]] || die "missing value for $1"
            mode="$2"
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

[[ -n "$issue_num" ]] || die "--issue is required"
[[ "$mode" == "check" || "$mode" == "sync" ]] || die "--mode must be check or sync"

command -v gh >/dev/null 2>&1 || die "gh CLI is required"

repo_args=()
if [[ -n "$repo" ]]; then
    repo_args=(--repo "$repo")
fi

issue_body="$(gh issue view "$issue_num" "${repo_args[@]}" --json body --jq .body)"
[[ -n "$issue_body" ]] || die "issue #${issue_num} has empty body"

# Gather all referenced issues in checklist lines.
declare -A referenced=()
while IFS= read -r line; do
    if [[ "$line" =~ ^([[:space:]]*)-[[:space:]]*\[([[:space:]xX])\][[:space:]]*#([0-9]+)(.*)$ ]]; then
        child_num="${BASH_REMATCH[3]}"
        referenced["$child_num"]=1
    fi
done <<< "$issue_body"

if [[ "${#referenced[@]}" -eq 0 ]]; then
    die "no checklist issue references found in #${issue_num}"
fi

# Cache issue state lookups.
declare -A state_for=()
for child_num in "${!referenced[@]}"; do
    child_state="$(gh issue view "$child_num" "${repo_args[@]}" --json state --jq .state)"
    if [[ "$child_state" != "OPEN" && "$child_state" != "CLOSED" ]]; then
        die "unexpected state for #${child_num}: ${child_state}"
    fi
    state_for["$child_num"]="$child_state"
done

new_body=""
changed=0
stale=0
checked_expected=0
unchecked_expected=0

while IFS= read -r line || [[ -n "$line" ]]; do
    out_line="$line"
    if [[ "$line" =~ ^([[:space:]]*)-[[:space:]]*\[([[:space:]xX])\][[:space:]]*#([0-9]+)(.*)$ ]]; then
        prefix="${BASH_REMATCH[1]}"
        current_mark="${BASH_REMATCH[2]}"
        child_num="${BASH_REMATCH[3]}"
        suffix="${BASH_REMATCH[4]}"

        child_state="${state_for[$child_num]}"
        expected_mark=" "
        if [[ "$child_state" == "CLOSED" ]]; then
            expected_mark="x"
            checked_expected=$((checked_expected + 1))
        else
            unchecked_expected=$((unchecked_expected + 1))
        fi

        normalized_current="$current_mark"
        if [[ "$normalized_current" == "X" ]]; then
            normalized_current="x"
        fi

        if [[ "$normalized_current" != "$expected_mark" ]]; then
            stale=$((stale + 1))
            out_line="${prefix}- [${expected_mark}] #${child_num}${suffix}"
            if [[ "$mode" == "sync" ]]; then
                changed=$((changed + 1))
            fi
        fi
    fi

    if [[ -z "$new_body" ]]; then
        new_body="$out_line"
    else
        new_body+=$'\n'
        new_body+="$out_line"
    fi
done <<< "$issue_body"

if [[ "$mode" == "sync" && "$changed" -gt 0 ]]; then
    printf '%s' "$new_body" | gh issue edit "$issue_num" "${repo_args[@]}" --body-file - >/dev/null
fi

echo "issue_tracker_checklist_sync: issue=#${issue_num} mode=${mode} refs=${#referenced[@]} stale=${stale} changed=${changed}"
echo "  expected_checked=${checked_expected} expected_unchecked=${unchecked_expected}"

if [[ "$mode" == "check" && "$stale" -gt 0 ]]; then
    die "found ${stale} stale checklist entries in #${issue_num}"
fi

exit 0
