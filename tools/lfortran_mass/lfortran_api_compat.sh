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
  --lfortran-build-llvm PATH
                          baseline LFortran LLVM build directory (default: <lfortran-dir>/build-llvm)
  --lfortran-build-liric PATH
                          WITH_LIRIC LFortran build directory (default: <lfortran-dir>/build-liric)
  --lfortran-repo URL     lfortran git URL (default: https://github.com/krystophny/lfortran.git)
  --lfortran-ref REF      lfortran git ref to checkout (default: origin/liric-aot-minimal)
  --lfortran-remote NAME  remote name for --lfortran-ref (default: origin)
  --skip-checkout         keep current lfortran checkout/ref
  --build-type TYPE       CMake build type (default: Release)
  --workers N             parallel build/test workers (default: nproc/sysctl)
  --run-ref-tests yes|no  run lfortran reference tests (default: yes)
  --run-itests yes|no     run lfortran integration tests (default: yes)
  --run-smoke-tests yes|no|auto
                          run llvm21 parity smoke tests (default: auto: yes for llvm21 family-set, else no)
  --run-third-party yes|no
                          run lfortran third-party suite (default: no)
  --third-party-lapack-mode MODE
                          third-party LAPACK mode: smoke|full (default: full)
  --itest-workers N       workers for integration tests only (default: 1)
  --itest-timeout-sec N   per integration-suite timeout in seconds (default: 900)
  --itest-memory-max SIZE memory cap for integration-suite scope, systemd format (default: 8G)
  --itest-tasks-max N     task cap for integration-suite scope (default: 512)
  --unsafe-itests         disable integration-suite containment guards
  --itest-shards LIST     semicolon-separated ctest regex shards for integration suites
  --itest-family-set SET  integration family set: legacy|quick|extended|llvm21 (default: legacy)
  --itest-families LIST   comma-separated integration families override
                          (llvm_base,llvm_fast,llvm_sc,llvm_submodule,llvm_single_invocation,llvm_std_f23,llvm2_base,llvm_rtlib_base,llvm_nopragma_base,llvm_integer8_base,llvm_implicit_base,llvm2_fast,llvm_rtlib_fast,llvm_nopragma_fast,llvm_integer8_fast,llvm_implicit_fast,llvm_submodule_sc)
  --env-name NAME         run test suites via conda/mamba env NAME (lf.sh style)
  --env-runner CMD        env runner for --env-name (auto: conda|micromamba|mamba)
  --ref-args "ARGS..."    extra args passed to ./run_tests.py
  --itest-args "ARGS..."  extra args passed to each integration_tests/run_tests.py invocation
  --skip-lfortran-build   do not rebuild selected lfortran build directories
  -h, --help              show this help

This lane validates compile-time API compatibility:
  LFortran built with -DWITH_LIRIC=yes uses Liric's LLVM C++ compatibility
  API internally, then runs LFortran's own test runners.
  Unit tests run via: ctest --test-dir <build-liric> --output-on-failure
  Integration tests run by family:
    legacy:
      run_tests.py -b llvm --ninja -jN
      run_tests.py -b llvm -f -nf16 --ninja -jN
    quick:
      legacy + run_tests.py -b llvm -sc --ninja -jN
    extended:
      quick +
      run_tests.py -b llvm_submodule --ninja -jN
      run_tests.py -b llvm_single_invocation --ninja -jN
      run_tests.py -b llvm --std=f23 --ninja -jN
    llvm21:
      extended +
      run_tests.py -b llvm2 --ninja -jN
      run_tests.py -b llvm_rtlib --ninja -jN
      run_tests.py -b llvm_nopragma --ninja -jN
      run_tests.py -b llvm_integer_8 --ninja -jN
      run_tests.py -b llvmImplicit --ninja -jN
      run_tests.py -b llvm2 -f --ninja -jN
      run_tests.py -b llvm_rtlib -f --ninja -jN
      run_tests.py -b llvm_nopragma -f --ninja -jN
      run_tests.py -b llvm_integer_8 -f --ninja -jN
      run_tests.py -b llvmImplicit -f --ninja -jN
      run_tests.py -b llvm_submodule -sc --ninja -jN
  (explicit --itest-families overrides --itest-family-set)
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

NO_LINK_FALLBACK_DIAG_TEXT="WITH_LIRIC AOT no-link executable emission failed"

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

detect_setsid_wait_flag() {
    if ! command -v setsid >/dev/null 2>&1; then
        printf '%s\n' ""
        return 0
    fi
    if setsid --help 2>&1 | grep -q -- '--wait'; then
        printf '%s\n' "--wait"
    else
        printf '%s\n' ""
    fi
}

detect_timeout_cmd() {
    if command -v timeout >/dev/null 2>&1; then
        printf '%s\n' "timeout"
    elif command -v gtimeout >/dev/null 2>&1; then
        printf '%s\n' "gtimeout"
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

clean_mod_artifacts() {
    local dir="$1"
    if [[ -d "$dir" ]]; then
        find "$dir" -maxdepth 1 -type f \( -name '*.mod' -o -name '*.smod' \) -delete
    fi
}

clean_dir_retry() {
    local dir="$1"
    local attempts="${2:-5}"
    local i
    for (( i = 1; i <= attempts; i++ )); do
        rm -rf "$dir" 2>/dev/null || true
        if [[ ! -e "$dir" ]]; then
            return 0
        fi
        find "$dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
        rmdir "$dir" 2>/dev/null || true
        if [[ ! -e "$dir" ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

clean_integration_build_tree() {
    local root="$1"
    local test_llvm_dir="${root}/test-llvm"
    if [[ -d "$test_llvm_dir" ]]; then
        if ! clean_dir_retry "$test_llvm_dir" 5; then
            echo "lfortran_api_compat: WARN: failed to fully clean ${test_llvm_dir}" >&2
        fi
    fi
}

is_nonneg_int() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

trim_ascii_ws() {
    local s="${1:-}"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s\n' "$s"
}

itest_args_has_filter() {
    local args="${1:-}"
    [[ "$args" == *" -t "* ]] && return 0
    [[ "$args" == -t* ]] && return 0
    [[ "$args" == *" --test "* ]] && return 0
    [[ "$args" == --test* ]] && return 0
    return 1
}

canonicalize_itest_family() {
    local token="${1,,}"
    token="${token//-/_}"
    case "$token" in
        llvm_base|base|default)
            printf '%s\n' "llvm_base"
            ;;
        llvm_fast|fast)
            printf '%s\n' "llvm_fast"
            ;;
        llvm_sc|llvm_separate_compilation|separate_compilation|sc)
            printf '%s\n' "llvm_sc"
            ;;
        llvm_submodule|submodule)
            printf '%s\n' "llvm_submodule"
            ;;
        llvm_single_invocation|single_invocation|single)
            printf '%s\n' "llvm_single_invocation"
            ;;
        llvm_std_f23|std_f23|f23)
            printf '%s\n' "llvm_std_f23"
            ;;
        llvm2_base|llvm2)
            printf '%s\n' "llvm2_base"
            ;;
        llvm_rtlib_base|rtlib|llvm_rtlib)
            printf '%s\n' "llvm_rtlib_base"
            ;;
        llvm_nopragma_base|nopragma|llvm_nopragma)
            printf '%s\n' "llvm_nopragma_base"
            ;;
        llvm_integer8_base|integer8|llvm_integer8|llvm_integer_8)
            printf '%s\n' "llvm_integer8_base"
            ;;
        llvm_implicit_base|implicit|llvm_implicit|llvmimplicit)
            printf '%s\n' "llvm_implicit_base"
            ;;
        llvm2_fast)
            printf '%s\n' "llvm2_fast"
            ;;
        llvm_rtlib_fast)
            printf '%s\n' "llvm_rtlib_fast"
            ;;
        llvm_nopragma_fast)
            printf '%s\n' "llvm_nopragma_fast"
            ;;
        llvm_integer8_fast)
            printf '%s\n' "llvm_integer8_fast"
            ;;
        llvm_implicit_fast)
            printf '%s\n' "llvm_implicit_fast"
            ;;
        llvm_submodule_sc|submodule_sc)
            printf '%s\n' "llvm_submodule_sc"
            ;;
        *)
            return 1
            ;;
    esac
}

populate_itest_families_from_set() {
    local set_name="${1,,}"
    set_name="${set_name//-/_}"
    case "$set_name" in
        legacy)
            itest_families_requested=(llvm_base llvm_fast)
            ;;
        quick)
            itest_families_requested=(llvm_base llvm_fast llvm_sc)
            ;;
        extended)
            itest_families_requested=(llvm_base llvm_fast llvm_sc llvm_submodule llvm_single_invocation llvm_std_f23)
            ;;
        llvm21)
            itest_families_requested=(
                llvm_base
                llvm_fast
                llvm_sc
                llvm_submodule
                llvm_single_invocation
                llvm_std_f23
                llvm2_base
                llvm_rtlib_base
                llvm_nopragma_base
                llvm_integer8_base
                llvm_implicit_base
                llvm2_fast
                llvm_rtlib_fast
                llvm_nopragma_fast
                llvm_integer8_fast
                llvm_implicit_fast
                llvm_submodule_sc
            )
            ;;
        *)
            die "--itest-family-set must be one of: legacy|quick|extended|llvm21"
            ;;
    esac
}

resolve_itest_families() {
    local seen=","
    local raw_item=""
    local canonical=""
    local -a parsed=()

    if [[ -n "$itest_families" ]]; then
        IFS=',' read -r -a parsed <<< "$itest_families"
        itest_families_requested=()
        for raw_item in "${parsed[@]}"; do
            raw_item="$(trim_ascii_ws "$raw_item")"
            [[ -n "$raw_item" ]] || continue
            canonical="$(canonicalize_itest_family "$raw_item")" \
                || die "unsupported integration family in --itest-families: ${raw_item}"
            if [[ "$seen" == *",${canonical},"* ]]; then
                continue
            fi
            seen="${seen}${canonical},"
            itest_families_requested+=("$canonical")
        done
    else
        populate_itest_families_from_set "$itest_family_set"
    fi

    ((${#itest_families_requested[@]} > 0)) \
        || die "no integration families resolved (check --itest-family-set/--itest-families)"
}

run_with_itest_guards() {
    local -a cmd=()
    if [[ "$safe_itests" != "yes" ]]; then
        run_in_selected_env "$@"
        return 0
    fi

    if [[ -n "$itest_memory_max" ]]; then
        if command -v systemd-run >/dev/null 2>&1 \
            && systemd-run --user --scope --quiet true >/dev/null 2>&1; then
            cmd+=(systemd-run --user --scope --quiet
                  -p "MemoryMax=${itest_memory_max}"
                  -p "TasksMax=${itest_tasks_max}")
        fi
    fi

    if [[ "$itest_pgroup_isolation" == "setsid" ]]; then
        cmd+=(setsid)
        if [[ -n "$setsid_wait_flag" ]]; then
            cmd+=("$setsid_wait_flag")
        fi
    fi

    if [[ "$itest_timeout_sec" -gt 0 ]]; then
        if [[ -n "$timeout_cmd" ]]; then
            cmd+=("$timeout_cmd" --signal=TERM --kill-after=15s "${itest_timeout_sec}s")
        fi
    fi

    if (( ${#cmd[@]} > 0 )); then
        run_in_selected_env "${cmd[@]}" "$@"
    else
        run_in_selected_env "$@"
    fi
}

run_with_itest_shards() {
    local mode_label="$1"
    shift
    local backend="$1"
    shift
    local -a mode_flags=("$@")
    local -a shard_patterns=("")
    local raw_pattern=""
    local pattern=""
    local -a cmd=()
    local shard_tmp_log=""
    local shard_idx=0
    local shard_count=1

    if [[ -n "$itest_shards" ]]; then
        IFS=';' read -r -a shard_patterns <<< "$itest_shards"
        shard_count="${#shard_patterns[@]}"
    fi

    for raw_pattern in "${shard_patterns[@]}"; do
        pattern="$(trim_ascii_ws "$raw_pattern")"
        shard_idx=$((shard_idx + 1))
        if [[ -n "$itest_shards" ]]; then
            if [[ -n "$pattern" ]]; then
                echo "lfortran_api_compat: integration shard ${shard_idx}/${shard_count} (${mode_label}): ${pattern}" >&2
            else
                echo "lfortran_api_compat: integration shard ${shard_idx}/${shard_count} (${mode_label}): <all-tests>" >&2
            fi
        fi
        cmd=("$PYTHON_BIN" run_tests.py -b "$backend" ${mode_flags[@]+"${mode_flags[@]}"} --ninja -j"$itest_workers")
        if [[ -n "$pattern" ]]; then
            cmd+=(-t "$pattern")
        fi
        shard_tmp_log="$(mktemp)"
        if ! run_with_itest_guards "${cmd[@]}" ${itest_extra[@]+"${itest_extra[@]}"} \
            2>&1 | tee "$shard_tmp_log"; then
            if [[ -n "$pattern" ]] && grep -Eq "No tests match pattern:|No tests were found!!!" "$shard_tmp_log"; then
                echo "lfortran_api_compat: integration shard ${shard_idx}/${shard_count} (${mode_label}) had no matching tests; skipping" >&2
                rm -f "$shard_tmp_log"
                continue
            fi
            rm -f "$shard_tmp_log"
            return 1
        fi
        rm -f "$shard_tmp_log"
    done
}

prepare_itest_env() {
    clean_mod_artifacts "$PWD"
    clean_integration_build_tree "$PWD"
    unset LFORTRAN_NO_LINK_MODULE_EMPTY_OBJECTS
    if [[ -z "${LFORTRAN_LINKER:-}" ]]; then
        export LFORTRAN_LINKER="gcc"
        echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_LINKER=${LFORTRAN_LINKER}" >&2
    fi
    if [[ -z "${LFORTRAN_NO_LINK_MODE:-}" ]]; then
        export LFORTRAN_NO_LINK_MODE="1"
        echo "lfortran_api_compat: applying WITH_LIRIC integration policy: LFORTRAN_NO_LINK_MODE=${LFORTRAN_NO_LINK_MODE}" >&2
    fi
    [[ "${LFORTRAN_NO_LINK_MODE}" == "1" ]] \
        || die "LFORTRAN_NO_LINK_MODE must remain 1 in WITH_LIRIC integration lane"
}

record_itest_family_result() {
    local family="$1"
    local family_status="$2"
    local duration_ms="$3"
    local log_file="$4"
    local pass_flag="false"
    if [[ "$family_status" -eq 0 ]]; then
        pass_flag="true"
    fi
    itest_family_result_entries+=("${family}|${family_status}|${pass_flag}|${duration_ms}|${log_file}")
}

run_integration_family() {
    local family="$1"
    local mode_label=""
    local backend=""
    local log_name=""
    local -a mode_flags=()
    local family_status=0
    local start_ts=0
    local end_ts=0
    local duration_ms=0

    case "$family" in
        llvm_base)
            mode_label="default"
            backend="llvm"
            log_name="lfortran_integration_tests_liric_api.log"
            ;;
        llvm_fast)
            mode_label="fast"
            backend="llvm"
            mode_flags=(-f -nf16)
            log_name="lfortran_integration_tests_liric_api_fast.log"
            ;;
        llvm_sc)
            mode_label="separate-compilation"
            backend="llvm"
            mode_flags=(-sc)
            log_name="lfortran_integration_tests_liric_api_sc.log"
            ;;
        llvm_submodule)
            mode_label="submodule"
            backend="llvm_submodule"
            log_name="lfortran_integration_tests_liric_api_submodule.log"
            ;;
        llvm_single_invocation)
            mode_label="single-invocation"
            backend="llvm_single_invocation"
            log_name="lfortran_integration_tests_liric_api_single_invocation.log"
            ;;
        llvm_std_f23)
            mode_label="std-f23"
            backend="llvm"
            mode_flags=(--std=f23)
            log_name="lfortran_integration_tests_liric_api_std_f23.log"
            ;;
        llvm2_base)
            mode_label="llvm2-default"
            backend="llvm2"
            log_name="lfortran_integration_tests_liric_api_llvm2.log"
            ;;
        llvm_rtlib_base)
            mode_label="llvm-rtlib-default"
            backend="llvm_rtlib"
            log_name="lfortran_integration_tests_liric_api_llvm_rtlib.log"
            ;;
        llvm_nopragma_base)
            mode_label="llvm-nopragma-default"
            backend="llvm_nopragma"
            log_name="lfortran_integration_tests_liric_api_llvm_nopragma.log"
            ;;
        llvm_integer8_base)
            mode_label="llvm-integer8-default"
            backend="llvm_integer_8"
            log_name="lfortran_integration_tests_liric_api_llvm_integer8.log"
            ;;
        llvm_implicit_base)
            mode_label="llvm-implicit-default"
            backend="llvmImplicit"
            log_name="lfortran_integration_tests_liric_api_llvm_implicit.log"
            ;;
        llvm2_fast)
            mode_label="llvm2-fast"
            backend="llvm2"
            mode_flags=(-f)
            log_name="lfortran_integration_tests_liric_api_llvm2_fast.log"
            ;;
        llvm_rtlib_fast)
            mode_label="llvm-rtlib-fast"
            backend="llvm_rtlib"
            mode_flags=(-f)
            log_name="lfortran_integration_tests_liric_api_llvm_rtlib_fast.log"
            ;;
        llvm_nopragma_fast)
            mode_label="llvm-nopragma-fast"
            backend="llvm_nopragma"
            mode_flags=(-f)
            log_name="lfortran_integration_tests_liric_api_llvm_nopragma_fast.log"
            ;;
        llvm_integer8_fast)
            mode_label="llvm-integer8-fast"
            backend="llvm_integer_8"
            mode_flags=(-f)
            log_name="lfortran_integration_tests_liric_api_llvm_integer8_fast.log"
            ;;
        llvm_implicit_fast)
            mode_label="llvm-implicit-fast"
            backend="llvmImplicit"
            mode_flags=(-f)
            log_name="lfortran_integration_tests_liric_api_llvm_implicit_fast.log"
            ;;
        llvm_submodule_sc)
            mode_label="submodule-separate-compilation"
            backend="llvm_submodule"
            mode_flags=(-sc)
            log_name="lfortran_integration_tests_liric_api_submodule_sc.log"
            ;;
        *)
            die "unsupported integration family: ${family}"
            ;;
    esac

    start_ts="$(date +%s)"
    (
        cd "$lfortran_dir/integration_tests"
        prepare_itest_env
        echo "lfortran_api_compat: running WITH_LIRIC integration suite (${mode_label}, backend=${backend})" >&2
        echo "lfortran_api_compat: integration guards: safe=${safe_itests} workers=${itest_workers} timeout_sec=${itest_timeout_sec} memory_max=${itest_memory_max} tasks_max=${itest_tasks_max} pgroup=${itest_pgroup_isolation}" >&2
        run_with_itest_shards "$mode_label" "$backend" "${mode_flags[@]}"
    ) 2>&1 | tee "${log_root}/${log_name}" || family_status=1
    if grep -Fq "$NO_LINK_FALLBACK_DIAG_TEXT" "${log_root}/${log_name}"; then
        echo "lfortran_api_compat: fallback diagnostic detected in integration family ${family}; refusing fallback" >&2
        family_status=1
    fi
    end_ts="$(date +%s)"
    duration_ms=$(( (end_ts - start_ts) * 1000 ))

    itest_families_executed+=("$family")
    record_itest_family_result "$family" "$family_status" "$duration_ms" "${log_root}/${log_name}"
    if [[ "$family_status" -ne 0 ]]; then
        status=1
    fi
}

run_smoke_tests() {
    local smoke_status=0
    (
        cd "$lfortran_dir"
        rm -f expr2.o expr2 intrinsics_04 intrinsics_04s
        echo "lfortran_api_compat: running WITH_LIRIC llvm21 smoke tests" >&2
        run_in_selected_env lfortran --version || exit 1

        run_in_selected_env lfortran -c examples/expr2.f90 -o expr2.o || exit 1
        run_in_selected_env lfortran -o expr2 expr2.o || exit 1
        [[ -x expr2 ]] || die "smoke test expected executable is missing: expr2"
        ./expr2 || exit 1

        echo "lfortran_api_compat: skipping mixed C/Fortran object-link smoke in mandatory no-link mode" >&2

        run_in_selected_env lfortran integration_tests/intrinsics_04s.f90 -o intrinsics_04s || exit 1
        [[ -x intrinsics_04s ]] || die "smoke test expected executable is missing: intrinsics_04s"
        ./intrinsics_04s || exit 1
        run_in_selected_env lfortran integration_tests/intrinsics_04.f90 -o intrinsics_04 || exit 1
        [[ -x intrinsics_04 ]] || die "smoke test expected executable is missing: intrinsics_04"
        ./intrinsics_04 || exit 1
    ) 2>&1 | tee "${log_root}/lfortran_smoke_tests_liric.log" || smoke_status=1
    if grep -Fq "$NO_LINK_FALLBACK_DIAG_TEXT" "${log_root}/lfortran_smoke_tests_liric.log"; then
        echo "lfortran_api_compat: fallback diagnostic detected in smoke tests; refusing fallback" >&2
        smoke_status=1
    fi

    if [[ "$smoke_status" -ne 0 ]]; then
        status=1
    fi
}

run_third_party_suite() {
    local third_party_status=0
    (
        cd "$lfortran_dir"
        echo "lfortran_api_compat: running WITH_LIRIC third-party suite (lapack-mode=${third_party_lapack_mode})" >&2
        run_in_selected_env env \
            RUNNER_OS="Linux" \
            FC="$lfortran_liric_bin" \
            LFORTRAN_LAPACK_TEST_MODE="$third_party_lapack_mode" \
            bash ci/test_third_party_codes.sh --lapack-mode "$third_party_lapack_mode"
    ) 2>&1 | tee "${log_root}/lfortran_third_party_liric.log" || third_party_status=1
    if grep -Fq "$NO_LINK_FALLBACK_DIAG_TEXT" "${log_root}/lfortran_third_party_liric.log"; then
        echo "lfortran_api_compat: fallback diagnostic detected in third-party suite; refusing fallback" >&2
        third_party_status=1
    fi

    if [[ "$third_party_status" -ne 0 ]]; then
        status=1
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

join_by_comma() {
    local IFS=','
    printf '%s' "$*"
}

json_array_from_list() {
    local json="["
    local sep=""
    local item=""
    for item in "$@"; do
        json+="${sep}\"$(json_escape "$item")\""
        sep=","
    done
    json+="]"
    printf '%s' "$json"
}

build_itest_family_results_json() {
    local json="{"
    local sep=""
    local entry=""
    local family=""
    local family_status=""
    local pass_flag=""
    local duration_ms=""
    local log_file=""

    for entry in "${itest_family_result_entries[@]}"; do
        IFS='|' read -r family family_status pass_flag duration_ms log_file <<< "$entry"
        json+="${sep}\"$(json_escape "$family")\":{"
        json+="\"status\":${family_status},"
        json+="\"pass\":${pass_flag},"
        json+="\"duration_ms\":${duration_ms},"
        json+="\"log\":\"$(json_escape "$log_file")\""
        json+="}"
        sep=","
    done
    json+="}"
    printf '%s' "$json"
}

write_summary_json() {
    local out="$1"
    local itest_families_requested_csv=""
    local itest_families_executed_csv=""
    local itest_families_requested_json="[]"
    local itest_families_executed_json="[]"
    local itest_family_results_json="{}"

    if ((${#itest_families_requested[@]} > 0)); then
        itest_families_requested_csv="$(join_by_comma "${itest_families_requested[@]}")"
        itest_families_requested_json="$(json_array_from_list "${itest_families_requested[@]}")"
    fi
    if ((${#itest_families_executed[@]} > 0)); then
        itest_families_executed_csv="$(join_by_comma "${itest_families_executed[@]}")"
        itest_families_executed_json="$(json_array_from_list "${itest_families_executed[@]}")"
    fi
    itest_family_results_json="$(build_itest_family_results_json)"

    if command -v jq >/dev/null 2>&1; then
        if jq -n \
            --arg lfortran_dir "$lfortran_dir" \
            --arg lfortran_ref "$lfortran_ref" \
            --arg lfortran_llvm_bin "$lfortran_llvm_bin" \
            --arg lfortran_bin "$lfortran_liric_bin" \
            --arg run_ref_tests "$run_ref_tests" \
            --arg run_itests "$run_itests" \
            --arg run_smoke_tests "$run_smoke_tests_flag" \
            --arg run_third_party "$run_third_party" \
            --arg third_party_lapack_mode "$third_party_lapack_mode" \
            --arg itest_family_set "$itest_family_set" \
            --arg itest_families_requested "$itest_families_requested_csv" \
            --arg itest_families_executed "$itest_families_executed_csv" \
            --argjson itest_family_results "$itest_family_results_json" \
            --arg workers "$workers" \
            --arg status "$status" \
            '{
              lfortran_dir: $lfortran_dir,
              lfortran_ref: $lfortran_ref,
              lfortran_llvm_bin: $lfortran_llvm_bin,
              lfortran_liric_bin: $lfortran_bin,
              run_ref_tests: $run_ref_tests,
              run_itests: $run_itests,
              run_smoke_tests: $run_smoke_tests,
              run_third_party: $run_third_party,
              third_party_lapack_mode: $third_party_lapack_mode,
              itest_family_set: $itest_family_set,
              itest_families_requested: (if $itest_families_requested == "" then [] else ($itest_families_requested|split(",")) end),
              itest_families_executed: (if $itest_families_executed == "" then [] else ($itest_families_executed|split(",")) end),
              itest_family_results: $itest_family_results,
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
    local esc_run_smoke_tests
    local esc_run_third_party
    local esc_third_party_lapack_mode
    local esc_itest_family_set
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
    esc_run_smoke_tests="$(json_escape "$run_smoke_tests_flag")"
    esc_run_third_party="$(json_escape "$run_third_party")"
    esc_third_party_lapack_mode="$(json_escape "$third_party_lapack_mode")"
    esc_itest_family_set="$(json_escape "$itest_family_set")"

    cat > "$out" <<EOF
{
  "lfortran_dir": "${esc_lfortran_dir}",
  "lfortran_ref": "${esc_lfortran_ref}",
  "lfortran_llvm_bin": "${esc_lfortran_llvm_bin}",
  "lfortran_liric_bin": "${esc_lfortran_bin}",
  "run_ref_tests": "${esc_run_ref_tests}",
  "run_itests": "${esc_run_itests}",
  "run_smoke_tests": "${esc_run_smoke_tests}",
  "run_third_party": "${esc_run_third_party}",
  "third_party_lapack_mode": "${esc_third_party_lapack_mode}",
  "itest_family_set": "${esc_itest_family_set}",
  "itest_families_requested": ${itest_families_requested_json},
  "itest_families_executed": ${itest_families_executed_json},
  "itest_family_results": ${itest_family_results_json},
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
lfortran_build_llvm_arg=""
lfortran_build_liric_arg=""
lfortran_repo="https://github.com/krystophny/lfortran.git"
lfortran_ref="origin/liric-aot-minimal"
lfortran_remote="origin"
build_type="Release"
workers="$(detect_workers)"
run_ref_tests="yes"
run_itests="yes"
run_smoke_tests_mode="${LIRIC_LFORTRAN_RUN_SMOKE_TESTS:-auto}"
run_smoke_tests_flag="no"
run_third_party="${LIRIC_LFORTRAN_RUN_THIRD_PARTY:-no}"
third_party_lapack_mode="${LIRIC_LFORTRAN_THIRD_PARTY_LAPACK_MODE:-full}"
itest_workers=""
itest_timeout_sec="${LIRIC_LFORTRAN_ITEST_TIMEOUT_SEC:-900}"
itest_memory_max="${LIRIC_LFORTRAN_ITEST_MEMORY_MAX:-8G}"
itest_tasks_max="${LIRIC_LFORTRAN_ITEST_TASKS_MAX:-512}"
safe_itests="yes"
itest_pgroup_isolation="none"
setsid_wait_flag=""
timeout_cmd=""
itest_shards="${LIRIC_LFORTRAN_ITEST_SHARDS:-}"
itest_family_set="${LIRIC_LFORTRAN_ITEST_FAMILY_SET:-legacy}"
itest_families="${LIRIC_LFORTRAN_ITEST_FAMILIES:-}"
env_name="${LIRIC_LFORTRAN_ENV_NAME:-}"
env_runner="${LIRIC_LFORTRAN_ENV_RUNNER:-}"
ref_args=""
itest_args=""
skip_lfortran_build="no"
skip_checkout="no"
fresh_workspace="no"
lfortran_with_runtime_stacktrace="${LIRIC_LFORTRAN_WITH_RUNTIME_STACKTRACE:-OFF}"
lfortran_ctest_exclude="${LIRIC_LFORTRAN_CTEST_EXCLUDE:-^test_lfortran$}"
itest_families_requested=()
itest_families_executed=()
itest_family_result_entries=()

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
        --lfortran-build-llvm)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_build_llvm_arg="$2"
            shift 2
            ;;
        --lfortran-build-liric)
            [[ $# -ge 2 ]] || die "missing value for $1"
            lfortran_build_liric_arg="$2"
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
        --run-smoke-tests)
            [[ $# -ge 2 ]] || die "missing value for $1"
            run_smoke_tests_mode="$2"
            shift 2
            ;;
        --run-third-party)
            [[ $# -ge 2 ]] || die "missing value for $1"
            run_third_party="$2"
            shift 2
            ;;
        --third-party-lapack-mode)
            [[ $# -ge 2 ]] || die "missing value for $1"
            third_party_lapack_mode="$2"
            shift 2
            ;;
        --itest-workers)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_workers="$2"
            shift 2
            ;;
        --itest-timeout-sec)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_timeout_sec="$2"
            shift 2
            ;;
        --itest-memory-max)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_memory_max="$2"
            shift 2
            ;;
        --itest-tasks-max)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_tasks_max="$2"
            shift 2
            ;;
        --unsafe-itests)
            safe_itests="no"
            shift
            ;;
        --itest-shards)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_shards="$2"
            shift 2
            ;;
        --itest-family-set)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_family_set="$2"
            shift 2
            ;;
        --itest-families)
            [[ $# -ge 2 ]] || die "missing value for $1"
            itest_families="$2"
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
[[ "$run_smoke_tests_mode" == "yes" || "$run_smoke_tests_mode" == "no" || "$run_smoke_tests_mode" == "auto" ]] \
    || die "--run-smoke-tests must be yes|no|auto"
[[ "$run_third_party" == "yes" || "$run_third_party" == "no" ]] \
    || die "--run-third-party must be yes|no"
[[ "$third_party_lapack_mode" == "smoke" || "$third_party_lapack_mode" == "full" ]] \
    || die "--third-party-lapack-mode must be smoke|full"
is_nonneg_int "$workers" || die "--workers must be a non-negative integer"
if [[ -z "$itest_workers" ]]; then
    itest_workers="1"
fi
is_nonneg_int "$itest_workers" || die "--itest-workers must be a non-negative integer"
is_nonneg_int "$itest_timeout_sec" || die "--itest-timeout-sec must be a non-negative integer"
is_nonneg_int "$itest_tasks_max" || die "--itest-tasks-max must be a non-negative integer"
if [[ -n "$itest_shards" ]] && itest_args_has_filter "$itest_args"; then
    die "--itest-shards cannot be combined with --itest-args containing -t/--test"
fi
if [[ "$run_itests" == "yes" ]]; then
    resolve_itest_families
fi
if [[ "$run_smoke_tests_mode" == "auto" ]]; then
    if [[ "$itest_family_set" == "llvm21" ]]; then
        run_smoke_tests_flag="yes"
    else
        run_smoke_tests_flag="no"
    fi
else
    run_smoke_tests_flag="$run_smoke_tests_mode"
fi

need_cmd git
need_cmd cmake
need_cmd ctest
if [[ "$safe_itests" == "yes" && "$run_itests" == "yes" && "$itest_timeout_sec" -gt 0 ]]; then
    timeout_cmd="$(detect_timeout_cmd)"
fi
if [[ "$safe_itests" == "yes" && "$run_itests" == "yes" ]]; then
    if command -v setsid >/dev/null 2>&1; then
        itest_pgroup_isolation="setsid"
        setsid_wait_flag="$(detect_setsid_wait_flag)"
    fi
fi

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
if [[ -n "$lfortran_build_llvm_arg" ]]; then
    lfortran_build_llvm="$(normalize_path "$lfortran_build_llvm_arg")"
else
    lfortran_build_llvm="${lfortran_dir}/build-llvm"
fi
if [[ -n "$lfortran_build_liric_arg" ]]; then
    lfortran_build_liric="$(normalize_path "$lfortran_build_liric_arg")"
else
    lfortran_build_liric="${lfortran_dir}/build-liric"
fi

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

if [[ "$skip_lfortran_build" != "yes" ]]; then
    lf_build_cc=""
    lf_build_cxx=""
    for _cc_cand in clang clang-21; do
        if command -v "$_cc_cand" >/dev/null 2>&1; then
            lf_build_cc="$_cc_cand"; break
        fi
    done
    for _cxx_cand in clang++ clang++-21; do
        if command -v "$_cxx_cand" >/dev/null 2>&1; then
            lf_build_cxx="$_cxx_cand"; break
        fi
    done
    lf_compiler_flags=()
    if [[ -n "$lf_build_cc" && -n "$lf_build_cxx" ]]; then
        lf_compiler_flags+=(-DCMAKE_C_COMPILER="$lf_build_cc" -DCMAKE_CXX_COMPILER="$lf_build_cxx")
        echo "lfortran_api_compat: using ${lf_build_cxx} for lfortran builds" >&2
    fi

    cmake -S "$lfortran_dir" -B "$lfortran_build_llvm" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        "${lf_compiler_flags[@]}" \
        2>&1 | tee "${log_root}/build_lfortran_llvm_configure.log"
    cmake --build "$lfortran_build_llvm" \
        --target lfortran lfortran_runtime build_runtime -j"$workers" \
        2>&1 | tee "${log_root}/build_lfortran_llvm_build.log"

    cmake -S "$lfortran_dir" -B "$lfortran_build_liric" -G Ninja \
        -DCMAKE_BUILD_TYPE="$build_type" \
        "${lf_compiler_flags[@]}" \
        -DWITH_LIRIC=yes \
        -DLIRIC_DIR="$liric_root" \
        -DWITH_RUNTIME_STACKTRACE="$lfortran_with_runtime_stacktrace" \
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

    [[ -n "$runtime_bc_clang" ]] || die "missing clang for WITH_LIRIC no-link runtime BC prebuild (set LIRIC_CLANG or install clang-21/clang)"

    runtime_src="${lfortran_dir}/src/libasr/runtime/lfortran_intrinsics.c"
    runtime_include="${lfortran_dir}/src"
    runtime_bc_out="${output_root}/liric_runtime.bc"
    [[ -f "$runtime_src" ]] || die "missing runtime source for WITH_LIRIC no-link BC prebuild: ${runtime_src}"

    echo "lfortran_api_compat: pre-building runtime BC with ${runtime_bc_clang}" >&2
    "$runtime_bc_clang" -O0 -emit-llvm -c "$runtime_src" \
        "-I${runtime_include}" -o "$runtime_bc_out"
    export LIRIC_RUNTIME_BC="$runtime_bc_out"
    echo "lfortran_api_compat: LIRIC_RUNTIME_BC=${LIRIC_RUNTIME_BC}" >&2
else
    [[ -f "${LIRIC_RUNTIME_BC}" ]] \
        || die "LIRIC_RUNTIME_BC is set but file does not exist: ${LIRIC_RUNTIME_BC}"
fi

[[ -n "${LIRIC_RUNTIME_BC:-}" ]] \
    || die "LIRIC_RUNTIME_BC must be set for WITH_LIRIC no-link mode"
[[ -f "${LIRIC_RUNTIME_BC}" ]] \
    || die "LIRIC_RUNTIME_BC file is missing: ${LIRIC_RUNTIME_BC}"

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

# test_lfortran exercises JIT-only multi-module flows that are out of scope
# for the current WITH_LIRIC AOT compatibility lane.
if [[ -n "$lfortran_ctest_exclude" ]]; then
    echo "lfortran_api_compat: excluding unit ctest pattern: ${lfortran_ctest_exclude}" >&2
    ctest --test-dir "$lfortran_build_liric" --output-on-failure \
        -E "$lfortran_ctest_exclude" \
        2>&1 | tee "${log_root}/lfortran_unit_ctest_liric.log" || status=1
else
    ctest --test-dir "$lfortran_build_liric" --output-on-failure \
        2>&1 | tee "${log_root}/lfortran_unit_ctest_liric.log" || status=1
fi

if [[ "$run_ref_tests" == "yes" ]]; then
    liric_ref_wrapper="${liric_root}/tools/lfortran_ref_test_liric.py"
    (
        cd "$lfortran_dir"
        # Remove stale mixed-version module artifacts that can poison
        # separate-compilation reference cases (for example submodule_04).
        clean_mod_artifacts "$lfortran_dir"
        clean_mod_artifacts "$lfortran_dir/integration_tests"
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

if [[ "$run_smoke_tests_flag" == "yes" ]]; then
    run_smoke_tests
fi

if [[ "$run_itests" == "yes" ]]; then
    itest_extra=()
    if [[ -n "$itest_args" ]]; then
        # shellcheck disable=SC2206
        itest_extra=( $itest_args )
    fi
    echo "lfortran_api_compat: integration family set=${itest_family_set}" >&2
    if [[ -n "$itest_families" ]]; then
        echo "lfortran_api_compat: integration families override=${itest_families}" >&2
    fi
    for itest_family in "${itest_families_requested[@]}"; do
        run_integration_family "$itest_family"
    done
fi

if [[ "$run_third_party" == "yes" ]]; then
    run_third_party_suite
fi

summary_json="${output_root}/summary.json"
write_summary_json "$summary_json"

if [[ "$status" -ne 0 ]]; then
    echo "lfortran_api_compat: FAILED (see ${log_root})" >&2
    exit "$status"
fi

echo "lfortran_api_compat: PASSED"
echo "summary: ${summary_json}"
