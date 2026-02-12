#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Run LLVM backend compatibility matrix in isolated temporary directories.

Usage:
  tools/llvm_backend_matrix.sh [--versions "7 8 ... 21"] [--keep]

Options:
  --versions LIST   Space-separated LLVM major versions (default: 7..21)
  --keep            Keep temporary work directory for debugging
  -h, --help        Show this help
EOF
}

versions="7 8 9 10 11 12 13 14 15 16 17 18 19 20 21"
keep=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --versions)
      shift
      versions="${1:-}"
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

for ver in ${versions}; do
  env_prefix="${work_root}/env-${ver}"
  build_dir="${work_root}/build-${ver}"
  with_compat="OFF"
  if [[ "${ver}" -ge 11 ]]; then
    with_compat="ON"
  fi
  echo
  echo "=== LLVM ${ver} (WITH_LLVM_COMPAT=${with_compat}) ==="
  micromamba create -y -p "${env_prefix}" -f "${env_file}" "llvmdev=${ver}" >/dev/null

  micromamba run -p "${env_prefix}" bash -lc "
    set -euo pipefail
    llvm_config=\$(command -v llvm-config)
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
echo "LLVM backend matrix finished successfully for versions: ${versions}"
