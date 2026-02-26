#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
usage: lfortran_api_compat.sh [options]
  --workspace PATH        workspace root for lfortran clone/build (default: <liric-build>/deps)
  --output-root PATH      output root for logs/artifacts (default: <liric-build>/deps/lfortran_api_compat_out)
  --liric-build-dir PATH  liric build dir for artifacts and defaults (default: $LIRIC_BUILD_DIR or <liric-root>/build)
  --fresh-workspace       remove existing workspace checkout/build and output root before running
  --lfortran-dir PATH     use an existing lfortran checkout (skip clone if present)
  --lfortran-repo URL     lfortran git URL (default: https://github.com/krystophny/lfortran.git)
  --lfortran-ref REF      lfortran git ref to checkout (default: origin/liric-aot-minimal)
  --lfortran-remote NAME  remote name for --lfortran-ref (default: origin)
  --skip-checkout         keep current lfortran checkout/ref
  --build-type TYPE       CMake build type (default: Release)
  --workers N             parallel build/test workers (default: nproc/sysctl)
  --run-ref-tests yes|no  run lfortran reference tests (default: yes)
  --run-itests yes|no     run lfortran integration tests (default: yes)
  --env-name NAME         run test suites via conda/mamba env NAME (lf.sh style)
  --env-runner CMD        env runner for --env-name (auto: conda|micromamba|mamba)
  --ref-args "ARGS..."    extra args passed to ./run_tests.py
  --itest-args "ARGS..."  extra args passed to each integration_tests/run_tests.py invocation
  --skip-lfortran-build   do not rebuild lfortran/build-llvm and lfortran/build-liric
  -h, --help              show this help

This lane validates compile-time API compatibility:
  LFortran built with -DWITH_LIRIC=yes uses Liric's LLVM C++ compatibility
  API internally, then runs LFortran's own test runners.
  Unit tests run via: ctest --test-dir <build-liric> --output-on-failure
  Integration tests run in both modes:
    run_tests.py -b llvm --ninja -jN
    run_tests.py -b llvm -f -nf16 --ninja -jN
  Reference tests run ALL backends via lfortran_ref_test_liric.py.
  The llvm backend (--show-llvm) skips IR output comparison (stderr and
  returncode still checked).  The run_dbg backend skips all comparison
  (debug-info/DWARF not yet implemented in liric).
  All other backends are fully compared.
EOF
}

die() {
    echo "lfortran_api_compat: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

find_env_runner() {
    if command -v conda >/dev/null 2>&1; then
        printf '%s\n' "conda"
    elif command -v micromamba >/dev/null 2>&1; then
        printf '%s\n' "micromamba"
    elif command -v mamba >/dev/null 2>&1; then
        printf '%s\n' "mamba"
    else
        printf '%s\n' ""
    fi
}

run_in_selected_env() {
    if [[ -n "$env_name" ]]; then
        local extra_args=()
        if [[ "$env_runner" == "conda" ]]; then
            extra_args+=(--no-capture-output)
        fi
        "$env_runner" run "${extra_args[@]}" -n "$env_name" "$@"
    else
        "$@"
    fi
}

require_tool_for_integration() {
    local tool="$1"
    if [[ -n "$env_name" ]]; then
        if ! run_in_selected_env bash -lc "command -v \"$tool\" >/dev/null 2>&1"; then
            die "missing required command in env '${env_name}': ${tool}; run ../lfortran-dev/scripts/lf.sh setup-env --llvm <version> or install LLVM tools in that env"
        fi
        return 0
    fi
    command -v "$tool" >/dev/null 2>&1 \
        || die "missing required command: ${tool}; set --env-name lf-llvm<version> (lf.sh style) or install LLVM tools in PATH"
}

ensure_lfortran_version_file() {
    local repo_dir="$1"
    local version_file="${repo_dir}/version"
    local version_value=""

    if [[ -f "$version_file" ]]; then
        return 0
    fi

    version_value="$(git -C "$repo_dir" describe --tags --always --dirty 2>/dev/null | sed 's/^v//')"
    [[ -n "$version_value" ]] || version_value="0.0.0-unknown"
    printf '%s\n' "$version_value" > "$version_file"
    echo "lfortran_api_compat: generated missing version file (${version_value})" >&2
}

ensure_lfortran_generated_sources() {
    local repo_dir="$1"
    local generated_preprocessor="${repo_dir}/src/lfortran/parser/preprocessor.cpp"
    local tag_count="0"
    local fallback_tag="v0.0.0-liric-aot-minimal"

    if [[ -f "$generated_preprocessor" ]]; then
        return 0
    fi

    tag_count="$(git -C "$repo_dir" tag --list | wc -l | tr -d '[:space:]')"
    if [[ "$tag_count" == "0" ]]; then
        if ! git -C "$repo_dir" rev-parse "$fallback_tag" >/dev/null 2>&1; then
            git -C "$repo_dir" tag "$fallback_tag" HEAD
            echo "lfortran_api_compat: created local fallback tag ${fallback_tag} for build0.sh" >&2
        fi
    fi

    need_cmd bash
    need_cmd re2c
    need_cmd bison
    echo "lfortran_api_compat: missing generated parser sources, running build0.sh" >&2
    (
        cd "$repo_dir"
        RE2C="$(command -v re2c)" BISON="$(command -v bison)" bash -e build0.sh
    )
}

ref_args_has_backend_policy() {
    local args="${1:-}"
    [[ "$args" == *"--no-llvm"* ]] && return 0
    [[ "$args" == *"--exclude-backend"* ]] && return 0
    [[ "$args" == *"--backend"* ]] && return 0
    [[ "$args" == *" -b "* ]] && return 0
    [[ "$args" == -b* ]] && return 0
    return 1
}

abs_path() {
    local p="$1"
    if [[ "$p" == /* ]]; then
        printf '%s\n' "$p"
        return 0
    fi
    printf '%s/%s\n' "$(pwd)" "$p"
}

normalize_path() {
    local p="$1"
    local parent
    local base

    p="$(abs_path "$p")"
    parent="$(dirname "$p")"
    base="$(basename "$p")"
    parent="$(cd "$parent" && pwd)"
    printf '%s/%s\n' "$parent" "$base"
}

json_escape() {
    local s="${1:-}"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\t'/\\t}"
    printf '%s' "$s"
}

write_summary_json() {
    local out="$1"

    if command -v jq >/dev/null 2>&1; then
        if jq -n \
            --arg lfortran_dir "$lfortran_dir" \
            --arg lfortran_ref "$lfortran_ref" \
            --arg lfortran_llvm_bin "$lfortran_llvm_bin" \
            --arg lfortran_bin "$lfortran_liric_bin" \
            --arg run_ref_tests "$run_ref_tests" \
            --arg run_itests "$run_itests" \
            --arg workers "$workers" \
            --arg status "$status" \
            '{
              lfortran_dir: $lfortran_dir,
              lfortran_ref: $lfortran_ref,
              lfortran_llvm_bin: $lfortran_llvm_bin,
              lfortran_liric_bin: $lfortran_bin,
              run_ref_tests: $run_ref_tests,
              run_itests: $run_itests,
              workers: ($workers|tonumber),
              status: ($status|tonumber),
              pass: (($status|tonumber) == 0)
            }' > "$out"; then
            return 0
        fi
        echo "lfortran_api_compat: WARN: jq summary generation failed; using shell fallback" >&2
    fi

    local esc_lfortran_dir
    local esc_lfortran_ref
    local esc_lfortran_llvm_bin
    local esc_lfortran_bin
    local esc_run_ref_tests
    local esc_run_itests
    local pass_flag="false"
    if [[ "$status" -eq 0 ]]; then
        pass_flag="true"
    fi
    esc_lfortran_dir="$(json_escape "$lfortran_dir")"
    esc_lfortran_ref="$(json_escape "$lfortran_ref")"
    esc_lfortran_llvm_bin="$(json_escape "$lfortran_llvm_bin")"
    esc_lfortran_bin="$(json_escape "$lfortran_liric_bin")"
    esc_run_ref_tests="$(json_escape "$run_ref_tests")"
    esc_run_itests="$(json_escape "$run_itests")"

    cat > "$out" <<EOF
{
  "lfortran_dir": "${esc_lfortran_dir}",
  "lfortran_ref": "${esc_lfortran_ref}",
  "lfortran_llvm_bin": "${esc_lfortran_llvm_bin}",
  "lfortran_liric_bin": "${esc_lfortran_bin}",
  "run_ref_tests": "${esc_run_ref_tests}",
  "run_itests": "${esc_run_itests}",
  "workers": ${workers},
  "status": ${status},
  "pass": ${pass_flag}
}
EOF
}

detect_workers() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        sysctl -n hw.ncpu
    fi
}

check_no_llvm_runtime_deps() {
    local bin="$1"
    local dep_out=""
    local lower=""
    local offending=""

    if [[ "$(uname)" == "Linux" ]]; then
        need_cmd ldd
        dep_out="$(ldd "$bin" || true)"
    elif [[ "$(uname)" == "Darwin" ]]; then
        need_cmd otool
        dep_out="$(otool -L "$bin" || true)"
    else
        echo "WARN: runtime dependency check skipped on unsupported OS: $(uname)" >&2
        return 0
    fi

    lower="$(printf '%s\n' "$dep_out" | tr '[:upper:]' '[:lower:]')"
    offending="$(printf '%s\n' "$lower" \
        | grep -E 'libllvm|/llvm[^[:space:]]*' || true)"
    if [[ -n "$offending" ]]; then
        echo "ERROR: detected LLVM runtime dependency in WITH_LIRIC binary: $bin" >&2
        printf '%s\n' "$dep_out" >&2
        return 1
    fi
    return 0
}

workspace=""
output_root=""
liric_build=""
lfortran_dir=""
lfortran_repo="https://github.com/krystophny/lfortran.git"
lfortran_ref="origin/liric-aot-minimal"
lfortran_remote="origin"
build_type="Release"
workers="$(detect_workers)"
run_ref_tests="yes"
run_itests="yes"
env_name="${LIRIC_LFORTRAN_ENV_NAME:-}"
env_runner="${LIRIC_LFORTRAN_ENV_RUNNER:-}"
ref_args=""
itest_args=""
skip_lfortran_build="no"
skip_checkout="no"
fresh_workspace="no"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workspace)
            [[ $# -ge 2 ]] || die "missing value for $1"
            workspace="$2"
            shift 2
            ;;
        --output-root)
            [[ $# -ge 2 ]] || die "missing value for $1"
            output_root="$2"
            shift 2
            ;;
        --liric-build-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            liric_build="$2"
            shift 2
            ;;
        --fresh-workspace)
            fresh_workspace="yes"
            shift
            ;;
        --lfortran-dir)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_dir="$2"
            shift 2
            ;;
        --lfortran-repo)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_repo="$2"
            shift 2
            ;;
        --lfortran-ref)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_ref="$2"
            shift 2
            ;;
        --lfortran-remote)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_remote="$2"
            shift 2
            ;;
        --build-type)
            [[ $# -ge 2 ]] || die "missing value for $1"
            build_type="$2"
            shift 2
            ;;
        --workers)
            [[ $# -ge 2 ]] || die "missing value for $1"
            workers="$2"
            shift 2
            ;;
        --run-ref-tests)
            [[ $# -ge 2 ]] || die "missing value for $1"
            run_ref_tests="$2"
            shift 2
            ;;
        --run-itests)
            [[ $# -ge 2 ]] || die "missing value for $1"
            run_itests="$2"
            shift 2
            ;;
        --env-name)
            [[ $# -ge 2 ]] || die "missing value for $1"
            env_name="$2"
            shift 2
            ;;
        --env-runner)
            [[ $# -ge 2 ]] || die "missing value for $1"
            env_runner="$2"
            shift 2
            ;;
        --ref-args)
            [[ $# -ge 2 ]] || die "missing value for $1"
            ref_args="$2"
            shift 2
            ;;
        --itest-args)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_args="$2"
            shift 2
            ;;
        --skip-lfortran-build)
            skip_lfortran_build="yes"
            shift
            ;;
        --skip-checkout)
            skip_checkout="yes"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ "$run_ref_tests" == "yes" || "$run_ref_tests" == "no" ]] || die "--run-ref-tests must be yes|no"
[[ "$run_itests" == "yes" || "$run_itests" == "no" ]] || die "--run-itests must be yes|no"

need_cmd git
need_cmd cmake
need_cmd ctest

if [[ -n "$env_name" ]]; then
    if [[ -z "$env_runner" ]]; then
        env_runner="$(find_env_runner)"
    fi
    [[ -n "$env_runner" ]] \
        || die "no conda/mamba/micromamba runner found for --env-name ${env_name}"
    need_cmd "$env_runner"
    if ! run_in_selected_env bash -lc "true" >/dev/null 2>&1; then
        die "unable to run commands in env '${env_name}' via ${env_runner}; run ../lfortran-dev/scripts/lf.sh setup-env --llvm <version>"
    fi
fi

PYTHON_BIN=""
if command -v python >/dev/null 2>&1; then
    PYTHON_BIN="python"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
else
    die "missing required command: python or python3"
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
liric_root="$(cd "${script_dir}/../.." && pwd)"

if [[ -z "$liric_build" ]]; then
    if [[ -n "${LIRIC_BUILD_DIR:-}" ]]; then
        liric_build="${LIRIC_BUILD_DIR}"
    else
        liric_build="${liric_root}/build"
    fi
fi
if [[ "$liric_build" != /* ]]; then
    liric_build="${liric_root}/${liric_build}"
fi
liric_build="$(normalize_path "$liric_build")"

if [[ -z "$workspace" ]]; then
    workspace="${liric_build}/deps"
fi
if [[ -z "$output_root" ]]; then
    output_root="${liric_build}/deps/lfortran_api_compat_out"
fi

mkdir -p "$workspace" "$output_root"
workspace="$(normalize_path "$workspace")"
output_root="$(normalize_path "$output_root")"

if [[ -z "$lfortran_dir" ]]; then
    lfortran_dir="${workspace}/lfortran"
fi
lfortran_dir="$(normalize_path "$lfortran_dir")"

if [[ "$fresh_workspace" == "yes" ]]; then
    rm -rf "$lfortran_dir" "$output_root"
fi

log_root="${output_root}/logs"
mkdir -p "$output_root" "$log_root"

if [[ ! -d "$lfortran_dir/.git" ]]; then
    git clone "$lfortran_repo" "$lfortran_dir" \
        2>&1 | tee "${log_root}/clone.log"
fi

if [[ "$skip_checkout" != "yes" ]]; then
    if [[ "$lfortran_ref" == "${lfortran_remote}/"* ]]; then
        if ! git -C "$lfortran_dir" remote | grep -qx "$lfortran_remote"; then
            git -C "$lfortran_dir" remote add "$lfortran_remote" "$lfortran_repo"
        fi
    fi

    git -C "$lfortran_dir" fetch --all --tags \
        2>&1 | tee "${log_root}/fetch.log"
    git -C "$lfortran_dir" checkout "$lfortran_ref" \
        2>&1 | tee "${log_root}/checkout.log"
fi

ensure_lfortran_version_file "$lfortran_dir" \
    2>&1 | tee "${log_root}/prepare_lfortran_version.log"
if [[ "$skip_lfortran_build" != "yes" ]]; then
    ensure_lfortran_generated_sources "$lfortran_dir" \
        2>&1 | tee "${log_root}/prepare_lfortran_generated_sources.log"
fi

if [[ ! -x "${liric_build}/liric_probe_runner" || ! -f "${liric_build}/libliric.a" ]]; then
    cmake -S "$liric_root" -B "$liric_build" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" \
        2>&1 | tee "${log_root}/build_liric_configure.log"
    cmake --build "$liric_build" -j"$workers" \
        2>&1 | tee "${log_root}/build_liric_build.log"
fi

lfortran_build_llvm="${lfortran_dir}/build-llvm"
lfortran_build_liric="${lfortran_dir}/build-liric"
if [[ "$skip_lfortran_build" != "yes" ]]; then
    cmake -S "$lfortran_dir" -B "$lfortran_build_llvm" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        2>&1 | tee "${log_root}/build_lfortran_llvm_configure.log"
    cmake --build "$lfortran_build_llvm" -j"$workers" \
        2>&1 | tee "${log_root}/build_lfortran_llvm_build.log"

    cmake -S "$lfortran_dir" -B "$lfortran_build_liric" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DWITH_LIRIC=yes \
        -DLIRIC_DIR="$liric_root" \
        -DWITH_RUNTIME_STACKTRACE=yes \
        -DWITH_LLVM=OFF \
        2>&1 | tee "${log_root}/build_lfortran_liric_configure.log"
    cmake --build "$lfortran_build_liric" -j"$workers" \
        2>&1 | tee "${log_root}/build_lfortran_liric_build.log"
fi

lfortran_llvm_bin="${lfortran_build_llvm}/src/bin/lfortran"
lfortran_liric_bin="${lfortran_build_liric}/src/bin/lfortran"
[[ -x "$lfortran_llvm_bin" ]] || die "lfortran baseline binary not found: $lfortran_llvm_bin"
[[ -x "$lfortran_liric_bin" ]] || die "lfortran binary not found: $lfortran_liric_bin"

check_no_llvm_runtime_deps "$lfortran_liric_bin" \
    2>&1 | tee "${log_root}/no_llvm_dep_check.log"

status=0

export PATH="${lfortran_build_liric}/src/bin:${PATH}"
resolved_lfortran="$(command -v lfortran || true)"
[[ -n "$resolved_lfortran" ]] || die "lfortran not found in PATH after WITH_LIRIC setup"
resolved_lfortran="$(normalize_path "$resolved_lfortran")"
if [[ "$resolved_lfortran" != "$lfortran_liric_bin" ]]; then
    die "PATH resolves lfortran to ${resolved_lfortran}; expected ${lfortran_liric_bin}"
fi

if [[ -z "${LIRIC_COMPILE_MODE:-}" ]]; then
    export LIRIC_COMPILE_MODE="isel"
    echo "lfortran_api_compat: applying WITH_LIRIC compile policy: LIRIC_COMPILE_MODE=${LIRIC_COMPILE_MODE}" >&2
fi
if [[ "${LIRIC_COMPILE_MODE}" == "llvm" ]]; then
    die "LIRIC_COMPILE_MODE=llvm is disallowed in WITH_LIRIC API compatibility lane"
fi
if [[ -z "${LIRIC_POLICY:-}" ]]; then
    export LIRIC_POLICY="direct"
    echo "lfortran_api_compat: applying WITH_LIRIC policy: LIRIC_POLICY=${LIRIC_POLICY}" >&2
fi

# Pre-build runtime BC once so individual lfortran invocations skip the
# per-process clang fork (~330ms each).  Mirrors compat_select_runtime_clang()
# and compat_build_runtime_bc() in src/liric_compat.c.
if [[ -z "${LIRIC_RUNTIME_BC:-}" ]]; then
    runtime_bc_clang=""
    for _cand in \
        "${LIRIC_CLANG:-}" \
        /opt/homebrew/opt/llvm/bin/clang-21 \
        /opt/homebrew/opt/llvm/bin/clang \
        /usr/local/opt/llvm/bin/clang-21 \
        /usr/local/opt/llvm/bin/clang \
        clang-21 \
        clang; do
        [[ -n "$_cand" ]] || continue
        if command -v "$_cand" >/dev/null 2>&1; then
            runtime_bc_clang="$_cand"
            break
        fi
    done
    if [[ -n "$runtime_bc_clang" ]]; then
        runtime_src="${lfortran_dir}/src/libasr/runtime/lfortran_intrinsics.c"
        runtime_include="${lfortran_dir}/src"
        runtime_bc_out="${output_root}/liric_runtime.bc"
        if [[ -f "$runtime_src" ]]; then
            echo "lfortran_api_compat: pre-building runtime BC with ${runtime_bc_clang}" >&2
            "$runtime_bc_clang" -O2 -emit-llvm -c "$runtime_src" \
                "-I${runtime_include}" -o "$runtime_bc_out"
            export LIRIC_RUNTIME_BC="$runtime_bc_out"
            echo "lfortran_api_compat: LIRIC_RUNTIME_BC=${LIRIC_RUNTIME_BC}" >&2
        else
            echo "lfortran_api_compat: runtime source not found, skipping BC pre-build" >&2
        fi
    else
        echo "lfortran_api_compat: no clang found, skipping runtime BC pre-build" >&2
    fi
fi

if [[ "$run_itests" == "yes" ]]; then
    # New stacktrace pipeline (no dwarfdump/python converters) writes
    # runtime debug maps in-process. Keep dwarfdump preflight only for
    # older LFortran trees that still carry converter scripts.
    if [[ -f "$lfortran_dir/src/libasr/dwarf_convert.py" || \
          -f "$lfortran_dir/src/libasr/dat_convert.py" ]]; then
        require_tool_for_integration "llvm-dwarfdump"
    else
        echo "lfortran_api_compat: llvm-dwarfdump preflight skipped (in-process debug-map pipeline detected)" >&2
    fi
fi

ctest --test-dir "$lfortran_build_liric" --output-on-failure \
    2>&1 | tee "${log_root}/lfortran_unit_ctest_liric.log" || status=1

if [[ "$run_ref_tests" == "yes" ]]; then
    liric_ref_wrapper="${liric_root}/tools/lfortran_ref_test_liric.py"
    (
        cd "$lfortran_dir"
        if [[ -z "${LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS:-}" ]]; then
            export LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS="1"
            echo "lfortran_api_compat: applying WITH_LIRIC reference policy: LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS=${LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS}" >&2
        fi
        export LIRIC_REF_SKIP_IR="llvm"
        export LIRIC_REF_SKIP_DBG="run_dbg"
        echo "lfortran_api_compat: IR comparison skipped for: ${LIRIC_REF_SKIP_IR}" >&2
        echo "lfortran_api_compat: debug-info comparison skipped for: ${LIRIC_REF_SKIP_DBG}" >&2
        if [[ -n "$ref_args" ]]; then
            # shellcheck disable=SC2086
            run_in_selected_env "$PYTHON_BIN" "$liric_ref_wrapper" $ref_args
        else
            run_in_selected_env "$PYTHON_BIN" "$liric_ref_wrapper"
        fi
    ) 2>&1 | tee "${log_root}/lfortran_reference_tests.log" || status=1
fi

if [[ "$run_itests" == "yes" ]]; then
    (
        cd "$lfortran_dir/integration_tests"
        unset LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS
        if [[ -z "${LFORTRAN_LINKER:-}" ]]; then
            export LFORTRAN_LINKER="gcc"
            echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_LINKER=${LFORTRAN_LINKER}" >&2
        fi
        if [[ -z "${LFORTRAN_NO_LINK_MODE:-}" ]]; then
            export LFORTRAN_NO_LINK_MODE="1"
            echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_NO_LINK_MODE=${LFORTRAN_NO_LINK_MODE}" >&2
        fi
        echo "lfortran_api_compat: running WITH_LIRIC integration suite (default mode)" >&2
        if [[ -n "$itest_args" ]]; then
            # shellcheck disable=SC2086
            run_in_selected_env "$PYTHON_BIN" run_tests.py -b llvm --ninja -j"$workers" $itest_args
        else
            run_in_selected_env "$PYTHON_BIN" run_tests.py -b llvm --ninja -j"$workers"
        fi
    ) 2>&1 | tee "${log_root}/lfortran_integration_tests_liric_api.log" || status=1

    (
        cd "$lfortran_dir/integration_tests"
        unset LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS
        if [[ -z "${LFORTRAN_LINKER:-}" ]]; then
            export LFORTRAN_LINKER="gcc"
            echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_LINKER=${LFORTRAN_LINKER}" >&2
        fi
        if [[ -z "${LFORTRAN_NO_LINK_MODE:-}" ]]; then
            export LFORTRAN_NO_LINK_MODE="1"
            echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_NO_LINK_MODE=${LFORTRAN_NO_LINK_MODE}" >&2
        fi
        echo "lfortran_api_compat: running WITH_LIRIC integration suite (-f -nf16 mode)" >&2
        if [[ -n "$itest_args" ]]; then
            # shellcheck disable=SC2086
            run_in_selected_env "$PYTHON_BIN" run_tests.py -b llvm -f -nf16 --ninja -j"$workers" $itest_args
        else
            run_in_selected_env "$PYTHON_BIN" run_tests.py -b llvm -f -nf16 --ninja -j"$workers"
        fi
    ) 2>&1 | tee "${log_root}/lfortran_integration_tests_liric_api_fast.log" || status=1
fi

summary_json="${output_root}/summary.json"
write_summary_json "$summary_json"

if [[ "$status" -ne 0 ]]; then
    echo "lfortran_api_compat: FAILED (see ${log_root})" >&2
    exit "$status"
fi

echo "lfortran_api_compat: PASSED"
echo "summary: ${summary_json}"
