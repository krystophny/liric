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
