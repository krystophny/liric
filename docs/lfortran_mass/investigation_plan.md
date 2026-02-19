# LFortran + Liric Investigation Plan

## Scope

Goal: make Liric a drop-in backend for LFortran with minimal LFortran branch deltas versus upstream `main`.

Two required compatibility lanes:

1. LLVM IR replay lane (`bench_compat_check` / `nightly_mass.sh`)
2. Compile-time API lane (`WITH_LIRIC` LFortran binary running LFortran test suites directly)

## Task Board Source

- Active checklist: `docs/lfortran_mass/failure_task_list.md`
- Taxonomy definitions: `docs/lfortran_failure_taxonomy.md`

## Investigation Order

1. `unsupported_abi` first (hard crashes / link ABI issues)
2. `mismatch` next (runtime semantic/output drift)
3. `lfortran_emit_fail` last and only if reproduced uniquely in `WITH_LIRIC` builds

## Per-Test Workflow

For each unchecked case in `failure_task_list.md`:

1. Reproduce in LLVM IR replay lane (`tools/lfortran_mass/nightly_mass.sh`)
2. Reproduce in compile-time API lane (`tools/lfortran_mass/lfortran_api_compat.sh`)
3. Classify ownership:
   - Liric runtime/JIT issue -> fix in `liric`
   - LFortran emitter/front-end issue -> keep minimal LFortran patch, prefer upstream-compatible change
4. Add/adjust a focused test in `liric` to lock regression
5. Re-run both lanes and check off case only when both pass

## Required Gates Before Marking Drop-In

1. `nightly_mass.sh` gate passes (`mismatch_count=0`, `unsupported_abi=0`)
2. `lfortran_api_compat.sh` passes reference + integration suites on CI matrix
3. No LLVM runtime deps in `WITH_LIRIC` LFortran binary (checked by `lfortran_api_compat.sh`)
