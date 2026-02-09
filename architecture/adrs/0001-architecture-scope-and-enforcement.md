# 1. Keep Architecture as Curated C4 + Generated Artifacts

## Status

Accepted

## Context

The project needs a stable architectural model that is easy to review and update,
without polluting git history with large generated outputs.

## Decision

- Keep curated architecture source in `architecture/workspace.dsl`.
- Generate derived artifacts (`architecture/generated`, `architecture/export`, `architecture/site`) on demand.
- Do not commit generated artifacts.
- Enforce generation and validation in CI (`tools/arch_check.sh`).

## Consequences

- Pros: clean diffs, explicit architecture ownership, reproducible outputs.
- Cons: requires local/CI tooling (Structurizr CLI, Graphviz for full static export).
