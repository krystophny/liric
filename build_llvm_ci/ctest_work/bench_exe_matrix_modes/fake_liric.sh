#!/usr/bin/env bash
set -euo pipefail
out=''
input=''
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        *) input="$1"; shift 1 ;;
    esac
done
base="$(basename "$input" .ll)"
case "$base" in
    ret42|add) rc=42 ;;
    arith_chain) rc=25 ;;
    loop_sum) rc=55 ;;
    fib20) rc=109 ;;
    *) rc=1 ;;
esac
echo "$LIRIC_COMPILE_MODE" >> "/home/ert/code/lfortran-dev/liric/build_llvm_ci/ctest_work/bench_exe_matrix_modes/logs/modes.log"
cat > "$out" <<SCRIPT
#!/usr/bin/env bash
exit $rc
SCRIPT
chmod +x "$out"
