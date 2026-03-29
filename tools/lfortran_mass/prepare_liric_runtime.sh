#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIRIC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LFORTRAN_DIR="${LFORTRAN_DIR:-${LIRIC_DIR}/../lfortran}"
BUILD_DIR="${LFORTRAN_BUILD_DIR:-${LFORTRAN_DIR}/build}"
WITH_LIRIC="${LFORTRAN_WITH_LIRIC:-0}"
BACKEND="${LIRIC_COMPILE_MODE:-copy_patch}"

if [[ "${WITH_LIRIC}" != "1" ]]; then
    exit 0
fi

runtime_src="${LFORTRAN_DIR}/src/libasr/runtime/lfortran_intrinsics.c"
runtime_include="${LFORTRAN_DIR}/src"
runtime_bc="${BUILD_DIR}/liric_runtime.bc"
runtime_archive="${BUILD_DIR}/liric_runtime.lrarch"
runtime_tool="${LIRIC_DIR}/build/liric_runtime_archive"

if [[ ! -x "${runtime_tool}" ]]; then
    echo "ERROR: missing liric runtime archive tool: ${runtime_tool}"
    echo "  Run: cmake --build ${LIRIC_DIR}/build -j\$(nproc)"
    exit 1
fi

if [[ ! -f "${runtime_src}" ]]; then
    echo "ERROR: missing runtime source: ${runtime_src}"
    exit 1
fi

find_clang() {
    local cand
    for cand in \
        "${LIRIC_CLANG:-}" \
        clang-21 clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 \
        clang-14 clang-13 clang-12 clang-11 clang; do
        [[ -n "${cand}" ]] || continue
        if command -v "${cand}" >/dev/null 2>&1; then
            command -v "${cand}"
            return 0
        fi
    done
    return 1
}

clang_bin="$(find_clang || true)"
if [[ -z "${clang_bin}" ]]; then
    echo "ERROR: clang is required to prepare the WITH_LIRIC runtime archive"
    exit 1
fi

mkdir -p "${BUILD_DIR}"

need_bc=0
if [[ ! -f "${runtime_bc}" || "${runtime_src}" -nt "${runtime_bc}" ]]; then
    need_bc=1
fi
if [[ ${need_bc} -eq 1 ]]; then
    "${clang_bin}" -O0 -emit-llvm -c "${runtime_src}" \
        -I"${runtime_include}" -o "${runtime_bc}"
fi

need_archive=0
if [[ ! -f "${runtime_archive}" || "${runtime_bc}" -nt "${runtime_archive}" || \
      "${runtime_tool}" -nt "${runtime_archive}" ]]; then
    need_archive=1
fi
if [[ ${need_archive} -eq 1 ]]; then
    "${runtime_tool}" --input-bc "${runtime_bc}" \
        --output "${runtime_archive}" \
        --backend "${BACKEND}"
fi

if [[ ! -f "${runtime_archive}" ]]; then
    echo "ERROR: failed to prepare WITH_LIRIC runtime archive: ${runtime_archive}"
    exit 1
fi
