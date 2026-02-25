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

P0 (UDIV/UREM/FREM) and P2 (semantic debug toggle removal) are complete.
17 files changed, 265 insertions, 60 deletions. 230/230 tests pass.

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

Formatter/runtime behavior in optimized modules needs re-validation after UDIV/UREM/FREM fix.

### 4.1 Stable minimal repros

Working case:
- `build/liric --jit /tmp/call_sff_link.bc`
- Output: `ab  cdef  ghi  jkl  qwerty 12`

Previously failing case (needs re-test):
- `build/liric --jit /tmp/call_sff_O1O1_link.bc`
- `build/deps/lfortran/build-liric/src/bin/lfortran --no-color /tmp/min_format_fail.f90`

`/tmp/min_format_fail.f90`:
```fortran
print '(A2,4(2X,A),I3)',"ab","cdef","ghi","jkl","qwerty",12
```

## 5. High-Value Findings

## 5.1 BINOP semantic coverage -- FIXED

UDIV/UREM/FREM opcodes added to IR, bitcode decoder, LL lexer/parser, all backends
(aarch64, x86_64, riscv64), wasm_to_ir, compat API, and constant folding.
Unsigned ops are no longer collapsed into signed ops.

## 5.2 Switch lowering and PHI predecessor remap -- PARTIALLY ADDRESSED

Semantic debug toggle `LIRIC_DBG_BC_DISABLE_SWITCH_PHI_FIXUPS` removed (it altered
semantics by skipping PHI remapping). Diagnostic toggle `LIRIC_DBG_BC_SWITCH` kept
(print-only, no semantic effect).

Still TODO: formal verifier checks after switch lowering in debug builds and
dedicated switch+phi regression tests.

## 5.3 Vararg call placement looked correct at call-site level

Prior triage verified stack/register vararg placement for the repro call looked ABI-correct (AArch64 Darwin).
Do not over-focus on call lowering until BINOP semantics + switch/phi correctness are closed.

## 6. Required Clean Solution Plan (No Hacks)

## P0: Fix IR semantic model to represent LLVM ops exactly -- DONE

All items complete:
- LR_OP_UDIV, LR_OP_UREM, LR_OP_FREM added to opcode enum
- Bitcode decoder maps to canonical LLVM numbering
- LL lexer/parser round-trips udiv/urem/frem correctly
- Constant folding with unsigned arithmetic + div-by-zero guards
- All three backends (aarch64, x86_64, riscv64) emit correct machine code
- WASM decoder uses correct unsigned opcodes
- Compat API (lc_create_udiv/urem/frem + CreateFRem) added
- 2 new parser tests + existing test assertions updated
- 230/230 tests pass

## P1: Make switch lowering + PHI remap formally correct -- OPEN

1. Audit and simplify predecessor remap invariants:
- exact predecessor multiset handling
- deterministic handling when old predecessor already present
- duplicate edge behavior correctness

2. Add internal verifier checks after switch lowering in debug/test builds:
- PHI predecessor count matches CFG predecessor count
- no dangling predecessor block IDs

3. Add minimal switch+phi regression tests extracted from LLVM21-optimized patterns.

## P2: Remove temporary diagnostic debt after closure -- DONE

- Semantic toggle `LIRIC_DBG_BC_DISABLE_SWITCH_PHI_FIXUPS` removed.
- Diagnostic toggle `LIRIC_DBG_BC_SWITCH` kept (print-only).

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
