# TODO: LIRIC Direct-Mode Closure Plan (Exact Next Actions)

Date: 2026-02-25
Repo: /Users/ert/code/liric
Branch: fix/lfortran-runtime-exe-emission
Current HEAD: 5f21f48 (`ir: add UDIV, UREM, FREM opcodes with full backend + decoder support`)

## 1. Non-Negotiable Constraints

- LIRIC must remain a drop-in LLVM 21 compatibility layer.
- Fixes belong in LIRIC, not LFortran downstream.
- Direct mode only for this lane; no hidden policy fallback.
- No no-link fallback removal in behavior: WITH_LIRIC executable flow stays no-link AOT.
- No hacks, no test-specific special casing, no quick patches that weaken semantics.

## 2. Checkpoint Snapshot

Local uncommitted code currently exists in:
- `src/bc_decode.c`
- `src/target_aarch64.c`
- `src/jit.c`

Purpose of those local edits:
- `bc_decode.c`: resolve call callee function metadata and set vararg/fixed-arg call metadata from resolved callee where needed.
- `target_aarch64.c`: normalize sub-32-bit integer `icmp` operands before compare so signed/unsigned predicates match LLVM semantics.
- `jit.c`: temporary debug instrumentation (`jit_debug_strlen`, env-gated symbol logging/wrapping).

## 3. What Is Confirmed Working

- Build compiles.
- Baseline linked bitcode repro executes correctly:
  - `build/liric --jit /tmp/call_sff_link.bc`
- Optimized parser failure on GEP compact form was previously fixed.

## 4. Primary Remaining Failure

Optimized formatter path still fails/hangs:
- `build/liric --jit /tmp/call_sff_O1_link.bc`
- `build/liric --jit /tmp/call_sff_O1O1_link.bc`

Observed behavior:
- Runtime mismatch (`Got argument of type (CHARACTER), while the format specifier is (I)`) and/or hang patterns in LFortran integration runs.
- In hanging executable samples (`format_26.out`, `format_34.out`), hotspots map predominantly inside `_lcompilers_string_format_fortran`-related code paths.

## 5. Exact Plan To Finish Cleanly

### P0. Freeze and sanitize observability (must do first)

1. Keep diagnostics only if they are non-semantic and env-gated.
2. Remove aborting debug hooks before final merge quality:
   - delete `jit_debug_strlen` wrapper path or move to a strict compile-time debug-only block not enabled in normal builds.
3. Keep `LIRIC_VERBOSE_*` logging only when zero semantic impact.

Acceptance:
- No default behavior change from diagnostics.
- No unconditional debug wrappers on libc/runtime symbols.

### P1. Isolate correctness delta between `call_sff_link.bc` and optimized modules

Commands:

```bash
# 1) Produce IR for triage
a=/tmp/call_sff_link.bc
b=/tmp/call_sff_O1_link.bc
c=/tmp/call_sff_O1O1_link.bc
/opt/homebrew/opt/llvm@21/bin/llvm-dis -o /tmp/sff_base.ll "$a"
/opt/homebrew/opt/llvm@21/bin/llvm-dis -o /tmp/sff_o1.ll "$b"
/opt/homebrew/opt/llvm@21/bin/llvm-dis -o /tmp/sff_o1o1.ll "$c"

# 2) Focus on formatter/runtime functions
rg -n "define .*(_lcompilers_string_format_fortran|parse_fortran_format|move_to_next_element|append_to_string_NTI|handle_integer)" /tmp/sff_*.ll

# 3) Run direct execution checks
build/liric --jit "$a"
build/liric --jit "$b"
build/liric --jit "$c"
```

Instructions:
- Compare optimized function bodies to baseline for opcode classes that may still be mis-modeled in direct AArch64 lowering.
- Prioritize integer width semantics, compares, select/phi interactions, and pointer/index arithmetic semantics under optimization.

Acceptance:
- A concrete IR pattern is identified that diverges between baseline and optimized runs.

### P2. Validate direct-mode semantics at IR->machine boundaries

1. Audit AArch64 lowering for the exact opcodes used in the divergent path.
2. For each suspected opcode, prove correctness for:
   - signed/unsigned behavior
   - sub-32-bit behavior (extension/truncation rules)
   - poison/undef-safe operational assumptions (do not invent non-LLVM semantics)
3. Add unit-level regression test(s) in LIRIC that exercise the minimal failing IR pattern.

Suggested target files:
- `src/target_aarch64.c`
- `src/ir_validate.c` (if invariant checks are needed)
- `tests/*` for new semantic regression coverage

Acceptance:
- `build/liric --jit /tmp/call_sff_O1_link.bc` and `...O1O1...` both produce correct output.
- New regression tests fail before fix and pass after fix.

### P3. Re-run LFortran API compat lane locally (authoritative)

Commands:

```bash
cmake --build build -j$(sysctl -n hw.ncpu)
ctest --test-dir build --output-on-failure
cmake --build build --target lfortran_api_compat -j$(sysctl -n hw.ncpu)
```

If compat stalls:

```bash
# Identify stuck tests quickly
ps -Ao pid,ppid,etime,command | rg "format_26|format_34|lfortran_api_compat|ctest"

# Reproduce one test with timeout wrapper
timeout 40 build/deps/lfortran/build-liric/src/bin/lfortran --no-color \
  build/deps/lfortran/src/lfortran/integration_tests/format_26.f90 -o /tmp/format_26.out
```

Acceptance:
- No hang in formatter integration tests.
- `lfortran_api_compat` target completes successfully.

### P4. Final cleanup and proof

1. Remove temporary diagnostics that are no longer needed.
2. Keep only permanent, low-overhead debug toggles with zero semantic effect.
3. Run benchmark matrix to confirm no direct-lane regression:

```bash
./build/bench_matrix --timeout 15
```

Acceptance:
- Clean diff with only semantic fixes + durable tests.
- All relevant tests green.
- Benchmarks complete; direct mode remains authoritative.

## 6. Commit Discipline

- Commit in small logical chunks:
  1. semantic fix
  2. regression tests
  3. cleanup
- Push each green checkpoint.
- Commit message format should state semantic area and proof target (example: `aarch64: fix i16 signed icmp normalization in direct lowering`).

## 7. Done Criteria

This task is complete only when all are true:
- Optimized linked formatter repros run correctly:
  - `build/liric --jit /tmp/call_sff_O1_link.bc`
  - `build/liric --jit /tmp/call_sff_O1O1_link.bc`
- LFortran compat target passes locally (`lfortran_api_compat`).
- No downstream LFortran hacks added for LIRIC compatibility.
- No semantic fallback paths added.
- Added regression tests cover the root-cause pattern.
