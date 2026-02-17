#!/usr/bin/env bash
set -euo pipefail
src=""
for arg in "$@"; do
  src="$arg"
done
if [[ "$src" == *"stop_case.f90" ]]; then
echo "STOP 3" >&2
exit 3
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
