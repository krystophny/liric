#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"${ROOT_DIR}/tools/arch_regen.sh"

required_files=(
    "${ROOT_DIR}/architecture/generated/file_inventory.tsv"
    "${ROOT_DIR}/architecture/generated/include_edges.tsv"
    "${ROOT_DIR}/architecture/generated/build_targets.tsv"
    "${ROOT_DIR}/architecture/generated/summary.tsv"
    "${ROOT_DIR}/architecture/site/index.html"
)

for file in "${required_files[@]}"; do
    if [[ ! -s "${file}" ]]; then
        echo "ERROR: expected generated file missing or empty: ${file}" >&2
        exit 1
    fi
done

echo "Architecture check passed"
