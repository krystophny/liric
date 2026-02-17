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

## Benchmarking (Canonical)

Use only `bench_matrix`:

```bash
./build/bench_matrix --iters 3 --timeout 15
```

Primitive lanes:
- `api_exe`, `api_jit`
- `ll_jit`, `ll_llvm`

Derived/compat lanes:
- `api_e2e` (compat alias; derived from `api_exe` and `api_jit`)
- `ll_e2e` and `ir_file` (derived from `ll_llvm` and `ll_jit`)

Canonical `--lanes all` includes only LLVM-relative lanes:
- `api_exe`, `api_jit`, `api_e2e`, `ll_jit`, `ll_llvm`, `ll_e2e`, `ir_file`
- `micro_c` is legacy and not part of canonical matrix coverage

Matrix axes:
- modes: `isel`, `copy_patch`, `llvm`
- policies: `direct`, `ir`

Artifacts:
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`
- `/tmp/liric_bench/matrix_skips.jsonl`
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
