# Liric

Liric is a C11 compiler/JIT for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

This repository uses one canonical benchmark driver: `bench_matrix`.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Benchmark Matrix Contract

Axes:
- `mode`: `isel`, `copy_patch`, `llvm`
- `policy`: `direct`, `ir`
- `lane`:
  `api_full_llvm`, `api_full_liric`,
  `api_backend_llvm`, `api_backend_liric`,
  `ll_jit`, `ll_llvm`

Lane meanings:
- `api_full_llvm`: full API wall/non-parse timing from lfortran LLVM build
- `api_full_liric`: full API wall/non-parse timing from lfortran WITH_LIRIC build
- `api_backend_llvm`: backend-isolated timing from lfortran LLVM build
- `api_backend_liric`: backend-isolated timing from lfortran WITH_LIRIC build
- `ll_jit`: liric LL timing (wall/non-parse ms)
- `ll_llvm`: LLVM LL timing (wall/non-parse ms)

Canonical `--lanes all` includes all lanes above plus `micro_c`.

## Run Matrix

```bash
./build/bench_matrix \
  --bench-dir /tmp/liric_bench \
  --modes all \
  --policies all \
  --lanes all \
  --iters 1 \
  --api-cases 100 \
  --timeout 15 \
  --timeout-ms 5000
```

Published benchmark artifacts:
- `docs/benchmarks/readme_perf_snapshot.json`
- `docs/benchmarks/readme_perf_table.md`

Regenerate published snapshot/table:

```bash
./tools/bench_readme_perf_snapshot.sh --build-dir ./build --bench-dir /tmp/liric_bench --out-dir docs/benchmarks
```

Lane tools (bench_matrix calls these internally):

```bash
./build/bench_compat_check --timeout 15
./build/bench_corpus_compare --iters 1 --policy direct
./build/bench_api --iters 1 --liric-policy direct
./build/bench_tcc --iters 10 --policy direct
```

Runtime artifacts:
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`
- `/tmp/liric_bench/matrix_summary.json`

## Speedup Tables (2026-02-17)

42/42 matrix cells OK. All timings are non-parse median milliseconds.

### API Backend (liric vs LLVM, 100 lfortran integration tests)

| Mode | Policy | LLVM (ms) | liric (ms) | Speedup |
|------|--------|----------:|-----------:|--------:|
| isel | direct | 9.79 | 1.49 | **6.6x** |
| isel | ir | 9.67 | 0.65 | **14.9x** |
| copy_patch | direct | 9.71 | 1.63 | **5.9x** |
| copy_patch | ir | 9.29 | 0.64 | **14.6x** |
| llvm | direct | 5.74 | 1.93 | **3.0x** |
| llvm | ir | 9.54 | 10.64 | 0.9x |

### LL Corpus (compile-only, 2187 .ll files)

| Mode | Policy | LLVM (ms) | liric (ms) | Speedup |
|------|--------|----------:|-----------:|--------:|
| isel | direct | 5.24 | 0.071 | **74x** |
| isel | ir | 5.29 | 0.072 | **74x** |
| copy_patch | direct | 5.29 | 0.073 | **73x** |
| copy_patch | ir | 5.24 | 0.071 | **74x** |
| llvm | direct | 5.13 | 6.12 | 0.84x |
| llvm | ir | 5.16 | 6.06 | 0.85x |

### Micro C (liric vs TCC, 5 micro-benchmarks)

| Mode | Policy | Speedup |
|------|--------|--------:|
| isel | direct | **5.5x** |
| isel | ir | **5.6x** |
| copy_patch | direct | **5.4x** |
| copy_patch | ir | **5.3x** |
| llvm | direct | 0.16x |
| llvm | ir | 0.16x |

The `llvm` mode rows use the real LLVM backend passthrough and are expected to be ~1x or slower.
