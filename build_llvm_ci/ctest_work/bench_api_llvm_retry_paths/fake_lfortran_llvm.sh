#!/usr/bin/env bash
set -euo pipefail
src=""
has_fast=0
for arg in "$@"; do
  if [[ "$arg" == "--fast" ]]; then
    has_fast=1
  fi
  src="$arg"
done
if [[ "$src" == *"io_case.f90" && ! -f "io_case_data.txt" ]]; then
  echo "Runtime error: File \`io_case_data.txt\` does not exists!"
  echo "Cannot open a file with the \`status=old\`"
  exit 1
fi
if [[ "$src" == *"pointee_case.f90" && "$has_fast" -eq 0 ]]; then
  echo "/tmp/lfortran_jit_ir_fake:163:14: error: explicit pointee type doesn't match operand's pointee type" >&2
  exit 1
fi
cat <<'OUT'
File reading: 1.0
Src -> ASR: 2.0
ASR passes (total): 3.0
ASR -> mod: 4.0
LLVM IR creation: 5.0
LLVM opt: 6.0
LLVM -> JIT: 7.0
JIT run: 8.0
Total time: 36.0
OUT
exit 0
