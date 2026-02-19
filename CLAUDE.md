# CLAUDE.md

Repository guidance for contributors and coding agents.

## Scope

Liric is a C11 JIT compiler for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

## Policy: C-Only Tooling

- Do not add new Python scripts.
- Prefer C tools and binaries for automation.
- If an old script-based workflow is replaced, keep only the C implementation.

## Build and Test

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## LFortran API Compatibility (WITH_LIRIC)

Run LFortran's own suites using a compile-time `WITH_LIRIC` build:

```bash
./tools/lfortran_mass/lfortran_api_compat.sh \
  --lfortran-ref origin/liric-aot \
  --workspace /tmp/liric_lfortran_api_compat \
  --output-root /tmp/liric_lfortran_api_compat_out
```

This checks that LFortran can run its native test runners while using Liric's
LLVM-compat API internally (not just `.ll` replay).

## Benchmarking (Canonical)

Use only `bench_matrix`:

```bash
./build/bench_matrix --timeout 15
```

Lanes:
- `api_full_llvm`, `api_full_liric`
- `api_backend_llvm`, `api_backend_liric`
- `ll_jit`, `ll_llvm`
- `micro_c`

Matrix axes:
- modes: `isel`, `copy_patch`, `llvm`
- policies: `direct`, `ir`

Artifacts:
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`
- `/tmp/liric_bench/matrix_summary.json`

## Source Map

- Core library: `src/*.c`, `include/liric/*.h`
- CLI: `tools/liric_main.c`
- Probe runner: `tools/liric_probe_runner.c`
- Benchmark runner: `tools/bench_matrix.c`
- Tests: `tests/*.c`, `tests/*.cpp`

## Coding Style

- C11
- 4-space indentation
- `lr_` prefix for public symbols
- keep benchmark semantics explicit and lane-separated
