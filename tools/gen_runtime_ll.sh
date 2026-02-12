#!/bin/bash
# Generate lfortran runtime as LLVM IR text (.ll) for embedding in liric executables.
# Requires: clang, lfortran source tree
# Usage: ./tools/gen_runtime_ll.sh <lfortran_src_dir> <output.ll>

set -euo pipefail

LFORTRAN_SRC="${1:?Usage: $0 <lfortran_src_dir> <output.ll>}"
OUTPUT="${2:?Usage: $0 <lfortran_src_dir> <output.ll>}"

RUNTIME_C="$LFORTRAN_SRC/src/libasr/runtime/lfortran_intrinsics.c"
if [ ! -f "$RUNTIME_C" ]; then
    echo "Error: $RUNTIME_C not found" >&2
    exit 1
fi

clang -S -emit-llvm -O0 -fno-exceptions -fno-unwind-tables \
    -I"$LFORTRAN_SRC/src" \
    -o "$OUTPUT" \
    "$RUNTIME_C"

FUNC_COUNT=$(grep -c '^\(define\|declare\)' "$OUTPUT")
echo "Generated $OUTPUT: $(wc -l < "$OUTPUT") lines, $FUNC_COUNT functions"
