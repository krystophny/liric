#!/usr/bin/env bash
set -euo pipefail

bench_script_name() {
    if [[ -n "${BENCH_SCRIPT_NAME:-}" ]]; then
        printf '%s' "${BENCH_SCRIPT_NAME}"
        return
    fi
    printf 'bench'
}

bench_die() {
    echo "$(bench_script_name): $*" >&2
    exit 1
}

bench_require_nonempty_file() {
    local path="$1"
    [[ -s "$path" ]] || bench_die "missing or empty file: ${path}"
}

bench_require_pattern() {
    local path="$1"
    local pattern="$2"
    local msg="$3"
    if ! grep -Eq "$pattern" "$path"; then
        bench_die "$msg (${path})"
    fi
}

bench_forbid_pattern() {
    local path="$1"
    local pattern="$2"
    local msg="$3"
    if grep -Eq "$pattern" "$path"; then
        bench_die "$msg (${path})"
    fi
}

bench_json_string_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || bench_die "missing string field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*"([^"]*)".*/\1/'
}

bench_json_int_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*[0-9]+" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || bench_die "missing integer field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/'
}

bench_json_bool_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*(true|false)" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || bench_die "missing boolean field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*(true|false).*/\1/'
}

bench_json_number_field() {
    local file="$1"
    local key="$2"
    local line
    line="$(grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*-?[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?" "$file" | head -n 1 || true)"
    [[ -n "$line" ]] || bench_die "missing numeric field '${key}' in ${file}"
    echo "$line" | sed -E 's/.*:[[:space:]]*(-?[0-9]+([.][0-9]+)?([eE][+-]?[0-9]+)?).*/\1/'
}

bench_json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

bench_to_abs_path() {
    local p="$1"
    if command -v realpath >/dev/null 2>&1; then
        realpath "$p"
        return
    fi
    if [[ "$p" == /* ]]; then
        printf '%s\n' "$p"
        return
    fi
    printf '%s/%s\n' "$(pwd)" "$p"
}

bench_fmt_fixed() {
    local val="$1"
    local digits="$2"
    awk -v v="$val" -v d="$digits" 'BEGIN { printf("%.*f", d, v + 0.0) }'
}

bench_require_executable() {
    local path="$1"
    [[ -x "$path" ]] || bench_die "missing executable: ${path}"
}

bench_find_runtime_lib() {
    local lfortran_src="$1"
    local first=""
    local preferred_llvm=""
    local preferred_liric=""
    local cand=""
    if [[ ! -d "$lfortran_src" ]]; then
        return 1
    fi
    while IFS= read -r cand; do
        if [[ -z "$first" ]]; then
            first="$cand"
        fi
        case "$cand" in
            */build-llvm/*)
                preferred_llvm="$cand"
                ;;
            */build-liric/*)
                preferred_liric="$cand"
                ;;
        esac
    done < <(find "$lfortran_src" -maxdepth 5 -type f \
        \( -name 'liblfortran_runtime.so' -o -name 'liblfortran_runtime.so.*' -o -name 'liblfortran_runtime.dylib' \) \
        | sort)
    if [[ -n "$preferred_llvm" ]]; then
        echo "$preferred_llvm"
        return 0
    fi
    if [[ -n "$preferred_liric" ]]; then
        echo "$preferred_liric"
        return 0
    fi
    if [[ -n "$first" ]]; then
        echo "$first"
        return 0
    fi
    return 1
}

bench_find_runtime_bc() {
    local lfortran_src="$1"
    local first=""
    local preferred_llvm=""
    local preferred_liric=""
    local cand=""
    if [[ ! -d "$lfortran_src" ]]; then
        return 1
    fi
    while IFS= read -r cand; do
        if [[ -z "$first" ]]; then
            first="$cand"
        fi
        case "$cand" in
            */build-llvm/*)
                preferred_llvm="$cand"
                ;;
            */build-liric/*)
                preferred_liric="$cand"
                ;;
        esac
    done < <(find "$lfortran_src" -maxdepth 6 -type f -name 'lfortran_intrinsics.bc' | sort)
    if [[ -n "$preferred_llvm" ]]; then
        echo "$preferred_llvm"
        return 0
    fi
    if [[ -n "$preferred_liric" ]]; then
        echo "$preferred_liric"
        return 0
    fi
    if [[ -n "$first" ]]; then
        echo "$first"
        return 0
    fi
    return 1
}
