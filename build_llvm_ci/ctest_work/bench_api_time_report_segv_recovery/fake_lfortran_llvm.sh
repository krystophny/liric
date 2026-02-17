#!/usr/bin/env bash
set -euo pipefail
src=""
has_time_report=0
for arg in "$@"; do
  if [[ "$arg" == "--time-report" ]]; then
    has_time_report=1
  fi
  src="$arg"
done
echo "llvm has_time_report=$has_time_report src=$src" >> "/home/ert/code/lfortran-dev/liric/build_llvm_ci/ctest_work/bench_api_time_report_segv_recovery/invocations.log"
if [[ "$has_time_report" -eq 0 ]]; then
  echo "program output"
  exit 0
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
