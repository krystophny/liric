# Architecture Model (C4)

This directory holds the curated C4 architecture source for liric.

- Source of truth (tracked): `architecture/workspace.dsl`
- Generated outputs (ignored):
  - `architecture/generated/`
  - `architecture/export/`
  - `architecture/site/`

## Local usage

```bash
./tools/arch_regen.sh
```

This will:
1. regenerate lightweight architecture metadata from the codebase,
2. validate the C4 workspace,
3. export diagrams (`mermaid`, `json`, and static HTML when Graphviz is available).

Open locally:

```bash
xdg-open architecture/site/index.html
```

If `dot` (Graphviz) is unavailable, regeneration still succeeds with a fallback index page and Mermaid exports.

## CI usage

CI runs `./tools/arch_check.sh` on every push/PR and uploads generated outputs as artifacts.
No generated diagram files are committed to git.
