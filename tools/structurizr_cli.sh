#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="${ROOT_DIR}/.cache/structurizr-cli"
ZIP_URL="https://github.com/structurizr/cli/releases/latest/download/structurizr-cli.zip"

mkdir -p "${CACHE_DIR}"

if [[ ! -x "${CACHE_DIR}/structurizr.sh" ]]; then
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "${tmp_dir}"' EXIT
    curl -fsSL -o "${tmp_dir}/structurizr-cli.zip" "${ZIP_URL}"
    unzip -q "${tmp_dir}/structurizr-cli.zip" -d "${tmp_dir}/cli"
    rm -rf "${CACHE_DIR}"
    mkdir -p "${CACHE_DIR}"
    cp -a "${tmp_dir}/cli/." "${CACHE_DIR}/"
    chmod +x "${CACHE_DIR}/structurizr.sh"
    rm -rf "${tmp_dir}"
    trap - EXIT
fi

exec "${CACHE_DIR}/structurizr.sh" "$@"
