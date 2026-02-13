#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Run LLVM backend compatibility matrix in isolated temporary directories.

Usage:
  tools/llvm_backend_matrix.sh [--versions "1 2 ... 22"] [--strict-versions] [--keep]

Options:
  --versions LIST   Space-separated LLVM major versions to request (default: 1..22)
  --strict-versions Fail if any requested version is unavailable/unsupported (default: skip)
  --list-available  Print conda-forge llvmdev majors and exit
  --keep            Keep temporary work directory for debugging
  -h, --help        Show this help
EOF
}

LIRIC_MIN_LLVM_MAJOR=5
LIRIC_MAX_LLVM_MAJOR=22
LIRIC_COMPAT_MIN_MAJOR=11

versions="$(seq 1 ${LIRIC_MAX_LLVM_MAJOR} | tr '\n' ' ')"
keep=0
strict_versions=0
list_available=0

discover_available_llvmdev_majors() {
  micromamba search -c conda-forge llvmdev --json \
    | sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' \
    | awk -F. '$1 ~ /^[0-9]+$/ {print $1}' \
    | sort -n -u
}

has_version() {
  local needle="$1"
  shift
  local candidate
  for candidate in "$@"; do
    if [[ "${candidate}" == "${needle}" ]]; then
      return 0
    fi
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --versions)
      shift
      versions="${1:-}"
      ;;
    --strict-versions)
      strict_versions=1
      ;;
    --list-available)
      list_available=1
      ;;
    --keep)
      keep=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if ! command -v micromamba >/dev/null 2>&1; then
  echo "micromamba is required" >&2
  exit 1
fi

mapfile -t available_versions < <(discover_available_llvmdev_majors)
if [[ "${#available_versions[@]}" -eq 0 ]]; then
  echo "No llvmdev versions discovered from conda-forge" >&2
  exit 1
fi

if [[ "${list_available}" -eq 1 ]]; then
  echo "Available llvmdev majors on conda-forge:"
  printf '%s\n' "${available_versions[@]}"
  exit 0
fi

requested_versions=()
for ver in ${versions}; do
  if [[ -n "${ver}" ]]; then
    requested_versions+=("${ver}")
  fi
done
if [[ "${#requested_versions[@]}" -eq 0 ]]; then
  echo "No versions requested" >&2
  exit 2
fi

runnable_versions=()
skipped_versions=()
for ver in "${requested_versions[@]}"; do
  if [[ ! "${ver}" =~ ^[0-9]+$ ]]; then
    echo "Invalid LLVM major version: ${ver}" >&2
    exit 2
  fi
  if (( ver < LIRIC_MIN_LLVM_MAJOR )); then
    reason="unsupported-by-liric(min=${LIRIC_MIN_LLVM_MAJOR})"
    if [[ "${strict_versions}" -eq 1 ]]; then
      echo "Requested llvmdev=${ver} is ${reason}" >&2
      exit 1
    fi
    skipped_versions+=("${ver}:${reason}")
    continue
  fi
  if (( ver > LIRIC_MAX_LLVM_MAJOR )); then
    reason="outside-requested-max(${LIRIC_MAX_LLVM_MAJOR})"
    if [[ "${strict_versions}" -eq 1 ]]; then
      echo "Requested llvmdev=${ver} is ${reason}" >&2
      exit 1
    fi
    skipped_versions+=("${ver}:${reason}")
    continue
  fi
  if ! has_version "${ver}" "${available_versions[@]}"; then
    reason="missing-on-conda-forge"
    if [[ "${strict_versions}" -eq 1 ]]; then
      echo "Requested llvmdev=${ver} is ${reason}" >&2
      exit 1
    fi
    skipped_versions+=("${ver}:${reason}")
    continue
  fi
  runnable_versions+=("${ver}")
done

if [[ "${#runnable_versions[@]}" -eq 0 ]]; then
  echo "No runnable LLVM versions after filtering." >&2
  echo "Requested: ${versions}" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${repo_root}/tools/llvm_backend_environment.yml"
if [[ ! -f "${env_file}" ]]; then
  echo "Missing environment file: ${env_file}" >&2
  exit 1
fi

work_root="$(mktemp -d /tmp/liric_llvm_matrix_XXXXXX)"
cleanup() {
  if [[ "${keep}" -eq 0 ]]; then
    rm -rf "${work_root}"
  else
    echo "kept work directory: ${work_root}"
  fi
}
trap cleanup EXIT

echo "work root: ${work_root}"
echo "requested llvmdev majors: ${versions}"
echo "available llvmdev majors: ${available_versions[*]}"
echo "runnable llvmdev majors: ${runnable_versions[*]}"
if [[ "${#skipped_versions[@]}" -gt 0 ]]; then
  echo "skipped llvmdev majors: ${skipped_versions[*]}"
fi

for ver in "${runnable_versions[@]}"; do
  env_prefix="${work_root}/env-${ver}"
  build_dir="${work_root}/build-${ver}"
  with_compat="OFF"
  if [[ "${ver}" -ge ${LIRIC_COMPAT_MIN_MAJOR} ]]; then
    with_compat="ON"
  fi
  echo
  echo "=== LLVM ${ver} (WITH_LLVM_COMPAT=${with_compat}) ==="
  if ! micromamba create -y -p "${env_prefix}" -f "${env_file}" "llvmdev=${ver}" >/dev/null; then
    if [[ "${strict_versions}" -eq 1 ]]; then
      echo "Failed to create environment for llvmdev=${ver}" >&2
      exit 1
    fi
    echo "Skipping llvmdev=${ver}: environment creation failed"
    continue
  fi

  micromamba run -p "${env_prefix}" bash -lc "
    set -euo pipefail
    llvm_config=\$(command -v llvm-config || true)
    if [[ -z \"\${llvm_config}\" ]]; then
      llvm_config=\$(command -v llvm-config-${ver} || true)
    fi
    if [[ -z \"\${llvm_config}\" ]]; then
      echo 'llvm-config not found in environment' >&2
      exit 1
    fi
    cmake -S '${repo_root}' -B '${build_dir}' -G Ninja \
      -DWITH_REAL_LLVM_BACKEND=ON \
      -DWITH_LLVM_COMPAT=${with_compat} \
      -DWITH_BENCH_TCC=OFF \
      -DWITH_BENCH_LLI_PHASES=OFF \
      -DLIRIC_LLVM_CONFIG_EXE=\"\${llvm_config}\"
    cmake --build '${build_dir}' -j\$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    ctest --test-dir '${build_dir}' --output-on-failure
  "
done

echo
echo "LLVM backend matrix finished successfully for runnable versions: ${runnable_versions[*]}"
