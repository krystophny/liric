#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/architecture/generated"
HAS_RG=0
if command -v rg >/dev/null 2>&1; then
    HAS_RG=1
fi

mkdir -p "${OUT_DIR}"

# 1) Inventory of core source files with LOC and owning subsystem.
{
    printf "path\tsubsystem\tloc\n"
    find "${ROOT_DIR}/src" "${ROOT_DIR}/include/liric" "${ROOT_DIR}/tools" "${ROOT_DIR}/tests" \
        -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
        | sed "s#^${ROOT_DIR}/##" \
        | sort \
        | while IFS= read -r path; do
            case "${path}" in
                src/ll_*|src/bc_decode.*|src/frontend_common.*|src/frontend_registry.*) subsystem="frontend-llvm" ;;
                src/wasm_*) subsystem="frontend-wasm" ;;
                src/jit.*) subsystem="jit" ;;
                src/target_*|src/target.h) subsystem="backend" ;;
                src/objfile*) subsystem="object" ;;
                src/liric_compat.*|include/liric/liric_compat.h|include/liric/liric_types.h|include/llvm/*) subsystem="compat" ;;
                src/ir.*|src/arena.*|src/builder.c) subsystem="core-ir" ;;
                src/liric.*|include/liric/liric.h) subsystem="api" ;;
                tools/*) subsystem="tools" ;;
                tests/*) subsystem="tests" ;;
                *) subsystem="other" ;;
            esac
            loc="$(wc -l < "${ROOT_DIR}/${path}" | tr -d '[:space:]')"
            printf "%s\t%s\t%s\n" "${path}" "${subsystem}" "${loc}"
        done
} > "${OUT_DIR}/file_inventory.tsv"

# 2) Local include dependency edges (C/C++ project includes only).
{
    printf "from\tinclude\n"
    find "${ROOT_DIR}/src" "${ROOT_DIR}/tools" "${ROOT_DIR}/tests" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
        | sed "s#^${ROOT_DIR}/##" \
        | sort \
        | while IFS= read -r path; do
            if [[ "${HAS_RG}" -eq 1 ]]; then
                includes="$(rg -No '^#include\\s+\"([^\"]+)\"' "${ROOT_DIR}/${path}" 2>/dev/null || true)"
            else
                includes="$(grep -hE '^#include[[:space:]]+\"[^\"]+\"' "${ROOT_DIR}/${path}" 2>/dev/null || true)"
            fi
            printf "%s\n" "${includes}" \
                | sed -E 's/^#include[[:space:]]+\"([^\"]+)\"/\1/' \
                | sort -u \
                | while IFS= read -r inc; do
                    [[ -z "${inc}" ]] && continue
                    printf "%s\t%s\n" "${path}" "${inc}"
                done
        done
} > "${OUT_DIR}/include_edges.tsv"

# 3) Build targets snapshot from CMakeLists.
{
    printf "target\tkind\n"
    if [[ "${HAS_RG}" -eq 1 ]]; then
        rg -No 'add_(library|executable)\(([^\\s\\)]+)' "${ROOT_DIR}/CMakeLists.txt" \
            | sed -E 's/add_(library|executable)\(([^\\s\\)]+)/\2\t\1/'
    else
        grep -Eo 'add_(library|executable)\([^[:space:])]+' "${ROOT_DIR}/CMakeLists.txt" \
            | sed -E 's/add_(library|executable)\(([^[:space:])]+)/\2\t\1/'
    fi \
        | sort -u
} > "${OUT_DIR}/build_targets.tsv"

# 4) Metadata summary for quick checks.
{
    printf "generated_at\t%s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf "inventory_rows\t%s\n" "$(($(wc -l < "${OUT_DIR}/file_inventory.tsv") - 1))"
    printf "include_edges\t%s\n" "$(($(wc -l < "${OUT_DIR}/include_edges.tsv") - 1))"
    printf "build_targets\t%s\n" "$(($(wc -l < "${OUT_DIR}/build_targets.tsv") - 1))"
} > "${OUT_DIR}/summary.tsv"
