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
echo "liric has_time_report=$has_time_report src=$src" >> "/home/ert/code/lfortran-dev/liric/build_llvm_ci/ctest_work/bench_api_time_report_segv_recovery/invocations.log"
if [[ "$has_time_report" -eq 1 && ("$src" == *"file_03.f90" || "$src" == *"file_08.f90") ]]; then
  kill -s SEGV $$
fi
if [[ "$has_time_report" -eq 0 ]]; then
  echo "program output"
  exit 0
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
