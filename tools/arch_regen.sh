#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH_DIR="${ROOT_DIR}/architecture"

"${ROOT_DIR}/tools/arch_extract.sh"

rm -rf "${ARCH_DIR}/export" "${ARCH_DIR}/site"
mkdir -p "${ARCH_DIR}/export" "${ARCH_DIR}/site"

"${ROOT_DIR}/tools/structurizr_cli.sh" validate -w "${ARCH_DIR}/workspace.dsl"

"${ROOT_DIR}/tools/structurizr_cli.sh" export -w "${ARCH_DIR}/workspace.dsl" -f mermaid -o "${ARCH_DIR}/export"
"${ROOT_DIR}/tools/structurizr_cli.sh" export -w "${ARCH_DIR}/workspace.dsl" -f json -o "${ARCH_DIR}/export"

if command -v dot >/dev/null 2>&1; then
    "${ROOT_DIR}/tools/structurizr_cli.sh" export -w "${ARCH_DIR}/workspace.dsl" -f static -o "${ARCH_DIR}/site"
else
    cat > "${ARCH_DIR}/site/index.html" <<'HTML'
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Liric Architecture Export</title>
</head>
<body>
  <h1>Liric Architecture Export</h1>
  <p>Graphviz (dot) is not installed, so full Structurizr static rendering was skipped.</p>
  <p>Install Graphviz and run <code>./tools/arch_regen.sh</code> to generate clickable static diagrams.</p>
  <ul>
    <li><a href="../export/workspace.json">workspace.json</a></li>
    <li><a href="../export/structurizr-SystemContext.mmd">SystemContext.mmd</a></li>
    <li><a href="../export/structurizr-Containers.mmd">Containers.mmd</a></li>
  </ul>
</body>
</html>
HTML
fi

echo "Architecture regeneration complete"
echo "  - Generated metadata: ${ARCH_DIR}/generated"
echo "  - Export files:       ${ARCH_DIR}/export"
echo "  - Static site:        ${ARCH_DIR}/site"
