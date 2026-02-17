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

Benchmark lanes:
- `api_exe`
- `api_jit`
- `ll_jit`
- `ll_lli`

Benchmark modes:
- `wall`
- `compile_only`
- `run_only`
- `parse_only`
- `non_parse`

Derived comparison lanes:
- `api_e2e` (derived from `api_exe` and `api_jit`)
- `ll_e2e` (derived from `ll_lli` and `ll_jit`)

Artifacts:
- `/tmp/liric_bench/bench_matrix_rows.jsonl`
- `/tmp/liric_bench/compat_api.txt`
- `/tmp/liric_bench/compat_ll.txt`
- `/tmp/liric_bench/summary.md`

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
