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
    if [[ -n "${LIRIC_CLANG:-}" ]]; then
        command -v "${LIRIC_CLANG}"
        return
    fi
    for cand in clang clang-21 clang-20 clang-19 clang-18 clang-17 clang-16 \
        clang-15 clang-14 clang-13 clang-12 clang-11; do
        if command -v "${cand}" >/dev/null 2>&1; then
            command -v "${cand}"
            return 0
        fi
    done
    return 1
}

clang_bin="$(find_clang || true)"
if [[ -z "${clang_bin}" ]]; then
    echo "ERROR: clang is required to prepare the WITH_LIRIC runtime archive" >&2
    exit 1
fi
clang_version="$("${clang_bin}" --version)"
roots="${LIRIC_RUNTIME_ROOTS:-}"
profile="${clang_bin}|${clang_version}|${roots}|${BACKEND}"
profile_file="${BUILD_DIR}/liric_runtime.profile"

find_matching_tool() {
    local tool="$1" cand version
    for cand in "$(dirname "${clang_bin}")/${tool}" "${tool}-${clang_major}" "${tool}"; do
        if command -v "${cand}" >/dev/null 2>&1; then
            cand="$(command -v "${cand}")"
            version="$("${cand}" --version)"
            if [[ "${version}" =~ version[[:space:]]+([0-9]+) ]] \
                && [[ "${BASH_REMATCH[1]}" == "${clang_major}" ]]; then
                printf '%s\n' "${cand}"
                return 0
            fi
        fi
    done
    echo "ERROR: LLVM ${clang_major} ${tool} is required for runtime-root selection" >&2
    return 1
}

if [[ -n "${roots}" ]]; then
    if [[ ! "${clang_version}" =~ version[[:space:]]+([0-9]+) ]]; then
        echo "ERROR: cannot identify the runtime clang version" >&2
        exit 1
    fi
    clang_major="${BASH_REMATCH[1]}"
    opt_bin="$(find_matching_tool opt)"
    dis_bin="$(find_matching_tool llvm-dis)"
    if [[ ! "${roots}" =~ ^[a-zA-Z_.$][a-zA-Z_0-9.$]*(,[a-zA-Z_.$][a-zA-Z_0-9.$]*)*$ ]]; then
        echo "ERROR: runtime roots must be a comma-separated list of symbols" >&2
        exit 1
    fi
    IFS=',' read -r -a root_names <<< "${roots}"
fi

mkdir -p "${BUILD_DIR}"
need_bc=0
if [[ ! -f "${runtime_bc}" || ! -f "${profile_file}" \
    || "$(cat "${profile_file}")" != "${profile}" \
    || "${BASH_SOURCE[0]}" -nt "${runtime_bc}" \
    || "${BUILD_DIR}/src/libasr/config.h" -nt "${runtime_bc}" ]]; then
    need_bc=1
fi
for dependency in "${LFORTRAN_DIR}/src/libasr/runtime/"*.[ch]; do
    if [[ "${dependency}" -nt "${runtime_bc}" ]]; then need_bc=1; fi
done
if [[ ${need_bc} -eq 1 ]]; then
    "${clang_bin}" -O0 -emit-llvm -c "${runtime_src}" \
        -I"${runtime_include}" -I"${BUILD_DIR}/src" -o "${runtime_bc}"
fi

need_archive=0
if [[ ${need_bc} -eq 1 || ! -f "${runtime_archive}" \
    || "${runtime_bc}" -nt "${runtime_archive}" || "${runtime_tool}" -nt "${runtime_archive}" ]]; then
    need_archive=1
fi
if [[ ${need_archive} -eq 1 ]]; then
    archive_bc="${runtime_bc}"
    if [[ -n "${roots}" ]]; then
        archive_bc="${BUILD_DIR}/liric_runtime_supported.bc"
        archive_ll="${BUILD_DIR}/liric_runtime_supported.ll"
        # Internalization retains initializer and function-pointer dependencies;
        # DCE then removes definitions and types outside the requested closure.
        "${opt_bin}" -passes=internalize,globaldce \
            "-internalize-public-api-list=${roots}" "${runtime_bc}" -o "${archive_bc}"
        "${dis_bin}" "${archive_bc}" -o "${archive_ll}"
        if ! awk '
            { text=$0; gsub(/"[^"]*"/,"",text); sub(/;.*/,"",text);
                if (text ~ /(^|[^a-zA-Z_0-9])(x86_fp80|fp128|ppc_fp128)([^a-zA-Z_0-9]|$)/) unsupported=1;
                while (match(text, /(^|[^a-zA-Z_0-9])i[0-9]+([^a-zA-Z_0-9]|$)/)) {
                token=substr(text,RSTART,RLENGTH); gsub(/[^0-9]/,"",token);
                if (token != "1" && token != "8" && token != "16" && token != "32" && token != "64") unsupported=1;
                text=substr(text,RSTART+RLENGTH-1);
            }}
            END { exit unsupported ? 1 : 0 }
        ' "${archive_ll}"; then
            echo "ERROR: requested runtime roots require unsupported integer or floating-point types" >&2
            exit 1
        fi
        for root in "${root_names[@]}"; do
            if ! awk -v root="${root}" '$1 == "define" && index($0, "@" root "(") { found=1 } END { exit found ? 0 : 1 }' "${archive_ll}"; then
                echo "ERROR: runtime root has no retained definition: ${root}" >&2
                exit 1
            fi
        done
        echo "Preparing runtime archive for roots: ${roots}"
    fi
    archive_tmp="$(mktemp "${runtime_archive}.XXXXXX")"
    trap 'rm -f "${archive_tmp}"' EXIT
    "${runtime_tool}" --input-bc "${archive_bc}" \
        --output "${archive_tmp}" --backend "${BACKEND}"
    mv "${archive_tmp}" "${runtime_archive}"
    trap - EXIT
    printf '%s\n' "${profile}" > "${profile_file}"
fi

if [[ ! -f "${runtime_archive}" ]]; then
    echo "ERROR: failed to prepare WITH_LIRIC runtime archive: ${runtime_archive}"
    exit 1
fi
