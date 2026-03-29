#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIRIC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LFORTRAN_DIR="${LFORTRAN_DIR:-${LIRIC_DIR}/../lfortran}"
LIRIC_BUILD_DIR="${LIRIC_BUILD_DIR:-${LIRIC_DIR}/build}"
LFORTRAN_LLVM_BUILD_DIR="${LFORTRAN_LLVM_BUILD_DIR:-${LFORTRAN_DIR}/build}"
LFORTRAN_BUILD_DIR="${LFORTRAN_BUILD_DIR:-${LFORTRAN_DIR}/build-liric}"
NPROC="${NPROC:-$(nproc)}"

liric_step() {
    printf '\n=== %s ===\n' "$1"
}

require_file() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        echo "ERROR: required path not found: ${path}"
        exit 1
    fi
}

require_command() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "ERROR: required command not found: ${cmd}"
        exit 1
    fi
}

ensure_layout() {
    require_file "${LFORTRAN_DIR}/CMakeLists.txt"
    require_file "${LIRIC_DIR}/CMakeLists.txt"
}

ensure_linux_deps() {
    require_command cmake
    require_command ninja
    require_command python3
    require_command python
    require_command clang
    require_command ctest
    require_command re2c
    require_command bison
}

bootstrap_lfortran_sources() {
    liric_step "Bootstrap lfortran generated sources"
    (
        cd "${LFORTRAN_DIR}"
        ./build0.sh
    )
}

ensure_llvm_dwarfdump() {
    if command -v llvm-dwarfdump >/dev/null 2>&1; then
        return 0
    fi

    local cand
    for cand in \
        llvm-dwarfdump-22 llvm-dwarfdump-21 llvm-dwarfdump-20 \
        llvm-dwarfdump-19 llvm-dwarfdump-18 llvm-dwarfdump-17 \
        llvm-dwarfdump-16 llvm-dwarfdump-15 llvm-dwarfdump-14 \
        llvm-dwarfdump-13 llvm-dwarfdump-12 llvm-dwarfdump-11; do
        if command -v "${cand}" >/dev/null 2>&1; then
            mkdir -p "${HOME}/.local/bin"
            ln -sf "$(command -v "${cand}")" "${HOME}/.local/bin/llvm-dwarfdump"
            export PATH="${HOME}/.local/bin:${PATH}"
            return 0
        fi
    done

    echo "ERROR: llvm-dwarfdump not found"
    exit 1
}

build_liric_release() {
    liric_step "Configure and build liric"
    cmake -S "${LIRIC_DIR}" -B "${LIRIC_BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${LIRIC_BUILD_DIR}" -j"${NPROC}"
}

build_lfortran_with_liric() {
    liric_step "Configure and build lfortran WITH_LIRIC"
    bootstrap_lfortran_sources
    cmake -S "${LFORTRAN_DIR}" -B "${LFORTRAN_BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DWITH_LIRIC=yes \
        -DWITH_LLVM=OFF \
        -DLIRIC_DIR="${LIRIC_DIR}" \
        -DWITH_RUNTIME_STACKTRACE=yes
    cmake --build "${LFORTRAN_BUILD_DIR}" -j"${NPROC}"
}

prepare_liric_runtime_archive() {
    liric_step "Prepare WITH_LIRIC runtime archive"
    LFORTRAN_WITH_LIRIC=1 \
    LFORTRAN_DIR="${LFORTRAN_DIR}" \
    LFORTRAN_BUILD_DIR="${LFORTRAN_BUILD_DIR}" \
        "${SCRIPT_DIR}/prepare_liric_runtime.sh"
}

run_lfortran_unit_subset() {
    liric_step "Run WITH_LIRIC unit/reference subset"
    "${LFORTRAN_BUILD_DIR}/src/lfortran/tests/test_lfortran" \
        --source-file-exclude='*test_llvm.cpp'
    ctest --test-dir "${LFORTRAN_BUILD_DIR}" --output-on-failure \
        -E '^test_lfortran$'
}

prepare_integration_build_tree() {
    liric_step "Prepare integration test build tree"
    rm -rf "${LFORTRAN_DIR}/integration_tests/build-lfortran-llvm"
    (
        cd "${LFORTRAN_DIR}/integration_tests"
        FC="${LFORTRAN_BUILD_DIR}/src/bin/lfortran" \
            cmake -DLFORTRAN_BACKEND=llvm -DCURRENT_BINARY_DIR=. \
            -S . -B build-lfortran-llvm
        cmake --build build-lfortran-llvm -j"${NPROC}"
    )
}

clean_integration_test_dirs() {
    local test_root="${LFORTRAN_DIR}/integration_tests"
    local attempt=0
    local path

    while (( attempt < 5 )); do
        local dirty=0
        shopt -s nullglob
        for path in "${test_root}"/test-*; do
            rm -rf "${path}" 2>/dev/null || true
            if [[ -e "${path}" ]]; then
                dirty=1
            fi
        done
        shopt -u nullglob
        if (( dirty == 0 )); then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done

    echo "ERROR: failed to clean integration test build trees under ${test_root}"
    exit 1
}

run_integration_suite() {
    (
        cd "${LFORTRAN_DIR}/integration_tests"
        clean_integration_test_dirs
        ./run_tests.py "$@"
    )
}

run_integration_common() {
    liric_step "Run common WITH_LIRIC integration parity"
    (
        cd "${LFORTRAN_DIR}/integration_tests"
        export LFORTRAN_TEST_ENV_VAR="STATUS OK!"
        ./run_tests.py -m
        ctest --test-dir build-lfortran-llvm -L llvm -j"${NPROC}" \
            --output-on-failure
    )
    run_integration_suite -b llvm llvm2 llvm_rtlib llvm_nopragma llvm_integer_8 llvmImplicit
    run_integration_suite -b llvm -sc
    run_integration_suite -b llvm2 llvm_rtlib llvm_nopragma llvm_integer_8 -f
    run_integration_suite -b llvm llvmImplicit -f
    run_integration_suite -b llvm_submodule
    run_integration_suite -b llvm_submodule -sc
}

run_integration_exhaustive_tail() {
    liric_step "Run exhaustive WITH_LIRIC integration parity"
    run_integration_suite -b llvm_single_invocation
    run_integration_suite -b llvm --std=f23
    run_integration_suite -b llvm -f --std=f23
}

run_quick_repo_checks() {
    liric_step "Run grammar and binary-history checks"
    (
        cd "${LFORTRAN_DIR}"
        ./ci/grammar_conflicts.sh
        python3 check_binary_file_in_git_history.py
    )
}
