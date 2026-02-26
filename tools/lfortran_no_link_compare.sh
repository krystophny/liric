#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: lfortran_no_link_compare.sh --lfortran-llvm PATH --lfortran-liric PATH --source PATH

Runs both lfortran variants in no-link AOT mode and reports output parity.
EOF
}

lfortran_llvm=""
lfortran_liric=""
source_path=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --lfortran-llvm)
      lfortran_llvm="${2:-}"
      shift 2
      ;;
    --lfortran-liric)
      lfortran_liric="${2:-}"
      shift 2
      ;;
    --source)
      source_path="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$lfortran_llvm" || -z "$lfortran_liric" || -z "$source_path" ]]; then
  usage >&2
  exit 2
fi

if [[ ! -x "$lfortran_llvm" ]]; then
  echo "missing executable: $lfortran_llvm" >&2
  exit 2
fi
if [[ ! -x "$lfortran_liric" ]]; then
  echo "missing executable: $lfortran_liric" >&2
  exit 2
fi
if [[ ! -f "$source_path" ]]; then
  echo "missing source file: $source_path" >&2
  exit 2
fi

tmp_dir="$(mktemp -d /tmp/lfortran_nolink_compare_XXXXXX)"
trap 'rm -rf "$tmp_dir"' EXIT

run_case() {
  local label="$1"
  local bin="$2"
  local out="$tmp_dir/${label}.out"
  local err="$tmp_dir/${label}.err"
  local rcfile="$tmp_dir/${label}.rc"

  set +e
  LFORTRAN_NO_LINK_MODE=1 "$bin" --no-color "$source_path" >"$out" 2>"$err"
  echo "$?" >"$rcfile"
  set -e
}

run_case llvm "$lfortran_llvm"
run_case liric "$lfortran_liric"

llvm_rc="$(cat "$tmp_dir/llvm.rc")"
liric_rc="$(cat "$tmp_dir/liric.rc")"

echo "source: $source_path"
echo "llvm_rc=$llvm_rc liric_rc=$liric_rc"

if cmp -s "$tmp_dir/llvm.out" "$tmp_dir/liric.out" && cmp -s "$tmp_dir/llvm.err" "$tmp_dir/liric.err"; then
  echo "result: output parity"
  exit 0
fi

echo "result: output mismatch"
echo "--- llvm stdout ---"
cat "$tmp_dir/llvm.out"
echo "--- llvm stderr ---"
cat "$tmp_dir/llvm.err"
echo "--- liric stdout ---"
cat "$tmp_dir/liric.out"
echo "--- liric stderr ---"
cat "$tmp_dir/liric.err"

exit 1
