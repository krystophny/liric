# TODO: LIRIC Direct-Mode Runtime/BC Correctness Closure (Checkpoint Handoff)

Date: 2026-02-25
Repository: /Users/ert/code/liric
Branch: fix/lfortran-runtime-exe-emission
Checkpoint base commit: 97b9457 (`jit: normalize LLVM \x01 external symbol names in provider lookup`)

## 1. Mission (Non-Negotiable)

LIRIC must be a drop-in LLVM 21 replacement via compat APIs, with clean implementation inside LIRIC.
No downstream hacks in LFortran are acceptable for LIRIC incompatibilities.

Hard requirements for this workstream:
- No hacks, no quick fixes, no special-casing individual test programs.
- No fallback to IR policy for this lane; direct mode must be correct.
- No linker fallback path for WITH_LIRIC executable generation.
- No semantic downgrades vs LLVM IR/BC behavior.

## 2. Current Checkpoint State

Local tree (uncommitted at handoff start) contained broad WIP across:
- src/bc_decode.c
- src/ir.c
- src/liric_compat.c
- src/objfile.c
- src/platform/platform_intrinsic_stubs_aarch64_darwin.S
- src/platform/platform_intrinsic_stubs_aarch64_linux.S
- src/platform/platform_intrinsic_stubs_x86_64_linux.S
- src/platform/platform_intrinsics.c
- src/target_aarch64.c
- src/target_x86_64.c

Diff size at checkpoint start:
- 10 files changed
- 1235 insertions, 240 deletions

## 3. What Was Confirmed Working

### 3.1 Optimized BC parse no longer fails at GEP record

A prior parse error for LLVM21 optimized linked module was fixed:
- Before: `parse error: malformed record: missing value operand`
- Trigger site: `FUNC_CODE_INST_GEP` in optimized runtime-heavy module.

Fix direction already in tree:
- Added fallback decoding for compact/value-only GEP operand form when value-type pair decode fails.
- Added record CHAR6 tracking (`record_is_char6`) to avoid mis-decoding char data.
- Added richer decoder error context (`func name`, `record code`) to improve triage.

Result:
- `build/liric --dump-ir /tmp/call_sff_O1O1_link.bc` now succeeds (no earlier parser abort).

## 4. What Is Still Failing (Main Blocker)

Formatter/runtime behavior is still wrong in optimized modules.

### 4.1 Stable minimal repros

Working case:
- `build/liric --jit /tmp/call_sff_link.bc`
- Output: `ab  cdef  ghi  jkl  qwerty 12`

Failing case:
- `build/liric --jit /tmp/call_sff_O1O1_link.bc`
- Behavior: unstable (runtime type mismatch and/or segfault depending on run/env)
- Typical failure:
  - `Runtime Error : Got argument of type (CHARACTER), while the format specifier is (I)`

LFortran reproducer:
- `build/deps/lfortran/build-liric/src/bin/lfortran --no-color /tmp/min_format_fail.f90`
- Same runtime format mismatch error.

`/tmp/min_format_fail.f90`:
```fortran
print '(A2,4(2X,A),I3)',"ab","cdef","ghi","jkl","qwerty",12
```

## 5. High-Value Findings (Critical)

## 5.1 BINOP semantic coverage is incomplete and currently wrong for LLVM opcodes

Current decoder mapping in `src/bc_decode.c` collapses unsigned ops into signed ops:
- integer opcode 3 and 4 both map to `SDIV`
- integer opcode 5 and 6 both map to `SREM`
- floating opcode 4 is mapped as `FDIV` (but LLVM binop 4 is `frem`)

This is not LLVM-correct and can corrupt optimized runtime logic.

Evidence from failing optimized module (`/tmp/call_sff_O1O1_link.bc`):
- module contains `udiv`, `urem`, and `frem` instructions.
- command used:
  - `/opt/homebrew/opt/llvm@21/bin/llvm-dis -o - /tmp/call_sff_O1O1_link.bc | rg -n "\budiv\b|\burem\b|\bfrem\b"`

Related architectural debt discovered:
- IR opcode set currently lacks dedicated `UDIV`, `UREM`, `FREM` opcodes.
- LL parser also maps `urem` token to signed remainder opcode (`SREM`).

This is a correctness bug cluster, not cosmetic.

## 5.2 Switch lowering and PHI predecessor remap remain suspect

Switch lowering in `bc_decode.c` rewrites switch into chains of compare+branch and then remaps PHI incoming preds.
Failure behavior changes when switch-phi fixups are disabled, indicating this path materially affects runtime behavior.

Current state has temporary debug toggles in decoder:
- `LIRIC_DBG_BC_SWITCH`
- `LIRIC_DBG_BC_DISABLE_SWITCH_PHI_FIXUPS`

Do not keep ad-hoc debug behavior as final solution. Keep only robust verifier-grade logic and optional diagnostics.

## 5.3 Vararg call placement looked correct at call-site level

Prior triage verified stack/register vararg placement for the repro call looked ABI-correct (AArch64 Darwin).
Do not over-focus on call lowering until BINOP semantics + switch/phi correctness are closed.

## 6. Required Clean Solution Plan (No Hacks)

## P0: Fix IR semantic model to represent LLVM ops exactly

1. Extend shared opcode enum with explicit:
- `LR_OP_UDIV`
- `LR_OP_UREM`
- `LR_OP_FREM`

2. Update all opcode consumers:
- IR validation/type checks (`src/ir.c`)
- textual name mapping and dumps
- parser/token mapping (`src/ll_parser.c`) so `urem` is unsigned, not signed
- any compat builder helpers if needed

3. Update all codegen backends in-tree:
- `src/target_x86_64.c`
- `src/target_aarch64.c`
- `src/target_riscv64.c` (if built/tested in this matrix)

Implement correct machine sequences for unsigned division/remainder and floating remainder.
No remapping unsigned semantics to signed.

4. Fix bitcode BINOP mapping in `bc_decode.c` to canonical LLVM numbering.

5. Add focused unit tests for each new opcode in direct mode.

## P1: Make switch lowering + PHI remap formally correct

1. Audit and simplify predecessor remap invariants:
- exact predecessor multiset handling
- deterministic handling when old predecessor already present
- duplicate edge behavior correctness

2. Add internal verifier checks after switch lowering in debug/test builds:
- PHI predecessor count matches CFG predecessor count
- no dangling predecessor block IDs

3. Add minimal switch+phi regression tests extracted from LLVM21-optimized patterns.

## P2: Remove temporary diagnostic debt after closure

- Keep useful logging hooks gated behind env vars if desired.
- Remove workaround-style toggles that alter semantics.

## P3: Re-run canonical validation matrix

Minimum required re-check:
- `build/liric --jit /tmp/call_sff_link.bc`
- `build/liric --jit /tmp/call_sff_O1_link.bc`
- `build/liric --jit /tmp/call_sff_O1O1_link.bc`
- `build/deps/lfortran/build-liric/src/bin/lfortran --no-color /tmp/min_format_fail.f90`
- LFortran format integration tests that previously timed out/failed (`format1`, `format_26`, `format_34`).

Then run broader suites:
- `cmake --build build -j$(sysctl -n hw.ncpu)`
- `ctest --test-dir build --output-on-failure`
- `./build/bench_matrix --timeout 15`

Performance/behavior policy:
- direct mode must stay authoritative for this lane
- no degradation via hidden fallback paths

## 7. Guardrails for Next LLM

Do NOT do any of the following:
- patch LFortran downstream to mask LIRIC bugs
- special-case `_lcompilers_string_format_fortran`
- force tests to pass by changing expected outputs
- add IR fallback/linker fallback to bypass direct-mode correctness

Do this instead:
- fix semantic completeness in LIRIC IR + decoder + backends
- prove correctness with minimized reproducer tests
- keep changes architecture-consistent and ABI-correct

## 8. Practical Triage Commands

Use these exact commands for quick signal:

```bash
# Parse check (optimized linked module)
build/liric --dump-ir /tmp/call_sff_O1O1_link.bc >/tmp/sff_dump.ll

# Behavior checks
build/liric --jit /tmp/call_sff_link.bc
build/liric --jit /tmp/call_sff_O1_link.bc
build/liric --jit /tmp/call_sff_O1O1_link.bc

# LFortran repro
build/deps/lfortran/build-liric/src/bin/lfortran --no-color /tmp/min_format_fail.f90

# Find unsigned/frem ops in BC module
/opt/homebrew/opt/llvm@21/bin/llvm-dis -o - /tmp/call_sff_O1O1_link.bc | rg -n "\\budiv\\b|\\burem\\b|\\bfrem\\b"
```

## 9. Definition of Done

This checkpoint thread is done only when all of the following are true:
- Optimized runtime-linked formatter repro produces correct output (no mismatch, no crash).
- LFortran format repro no longer errors.
- LFortran unit/integration tests for affected area pass in WITH_LIRIC direct mode.
- No downstream hacks added.
- No quick-fix toggles left as semantic crutches.
- Changes are cleanly reviewable and LLVM-compat semantics are explicit.
