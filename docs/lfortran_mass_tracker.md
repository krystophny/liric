# LFortran Mass Support Tracker

## Scope
This tracker measures compatibility against LFortran tests that are both:
- LLVM-intended
- not expected-failure/error-handling tests

Corpora:
- `../lfortran/tests/tests.toml`
- `../lfortran/integration_tests/CMakeLists.txt` `RUN(...)` entries with `llvm` and not `FAIL`

## Baseline
Baseline source: `/private/tmp/liric_lfortran_mass/summary.md`

| Metric | Baseline |
|---|---:|
| selected | 2415 |
| emit | 2400 |
| parse | 1177 |
| jit | 1 |
| unsupported_abi | 1174 |
| unsupported_feature | 945 |
| liric_parse_fail | 278 |
| lfortran_emit_fail | 15 |

## Current Snapshot
Latest measured snapshot:

| Metric | Current | Delta vs baseline |
|---|---:|---:|
| selected | 2415 | +0 |
| emit | 2414 | +14 |
| parse | 1186 | +9 |
| jit | 1 | +0 |
| unsupported_abi | 1183 | -9 |
| unsupported_feature | 950 | -5 |
| liric_parse_fail | 278 | +0 |
| lfortran_emit_fail | 1 | +14 |

## Issue Order and Ownership
Owner mapping is by technical area; assignees can be adjusted in GitHub:

| Order | Issue | Area owner |
|---:|---|---|
| 1 | #6 Harness: extrafile compilation | Harness |
| 2 | #7 Harness: filter/skip accounting | Harness |
| 3 | #8 Parser: modern operand grammar | Parser |
| 4 | #9 Parser: string/global forms | Parser |
| 5 | #10 Parser+IR: phi/select forms | Parser+IR |
| 6 | #11 Codegen: floating-point scalar ops | Codegen |
| 7 | #12 Codegen: phi/select lowering | Codegen |
| 8 | #13 JIT data model globals/strings | JIT |
| 9 | #14 Runtime ABI symbol integration | Runtime |
| 10 | #15 External/varargs call ABI | Runtime |
| 11 | #16 Differential parity lane | Harness |
| 12 | #17 Nightly CI trend artifacts | CI |
| 13 | #18 Commit message policy docs | Docs |

## Milestone Close Criteria
- `lfortran_emit_fail = 0`
- `liric_parse_fail = 0`
- `unsupported_feature = 0`
- `unsupported_abi = 0`
- `liric_jit_fail = 0`
- `pass == selected`
- Differential lane active for runnable-selected cases with `mismatch = 0`

## Commit Message Policy
Every commit for roadmap issues must end with:

`MassTest: selected <S>, emit <E> (+dE), parse <P> (+dP), jit <J> (+dJ), diff_match <D> (+dD)`
