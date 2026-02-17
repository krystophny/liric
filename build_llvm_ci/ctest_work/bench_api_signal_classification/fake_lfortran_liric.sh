#!/usr/bin/env bash
set -euo pipefail
src=""
for arg in "$@"; do
  src="$arg"
done
if [[ "$src" == *"segv_case.f90" ]]; then
  kill -s SEGV $$
fi
cat <<'OUT'
File reading: 1.5
Src -> ASR: 2.5
ASR passes (total): 3.5
ASR -> mod: 4.5
LLVM IR creation: 5.5
LLVM opt: 6.5
LLVM -> JIT: 7.5
JIT run: 8.5
Total time: 40.0
OUT
exit 0
