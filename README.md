# Liric

Liric is a C11 compiler/JIT for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

This repo uses one canonical benchmark driver: `bench_matrix`.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Benchmark Matrix

Axes:
- `mode`: `isel`, `copy_patch`, `llvm`
- `policy`: `direct`, `ir`
- `lane`: `api_exe`, `api_jit`, `api_e2e`, `ll_jit`, `ll_llvm`, `ll_e2e`, `ir_file`

Lane meanings:
- `api_exe`: LLVM-side API timing (wall/non-parse ms)
- `api_jit`: liric-side API timing (wall/non-parse ms)
- `api_e2e`: derived API speedup (`api_exe / api_jit`)
- `ll_jit`: liric LL timing (wall/non-parse ms)
- `ll_llvm`: LLVM LL timing (wall/non-parse ms)
- `ll_e2e`: derived LL speedup (`ll_llvm / ll_jit`)
- `ir_file`: compatibility alias of `ll_e2e`

`api_e2e`, `ll_e2e`, and `ir_file` are speedup lanes.

## Latest Snapshot

Generated: `2026-02-17T10:26:15Z`

Run configuration:

```bash
# compatibility set
build/bench_compat_check --bench-dir /tmp/liric_bench_main_full_1771322625 --timeout 15 --limit 350

# full matrix (all modes, all policies, all canonical lanes)
build/bench_matrix \
  --bench-dir /tmp/liric_bench_main_full_1771322625 \
  --skip-compat-check --skip-lfortran-rebuild \
  --modes all --policies all --lanes all \
  --iters 1 --api-cases 100 --timeout 15 --timeout-ms 5000
```

Dataset/accounting:
- `compat_api`: 238
- `compat_ll`: 101
- matrix cells: `42 attempted`, `34 ok`, `8 failed`
- all 8 failures are LL lanes in `mode=llvm` (`bench_corpus_compare_failed`)

### Full Cell Table

`Metric 1/2`:
- timing lanes: `wall_ms` / `non_parse_ms`
- speedup lanes: `wall_speedup` / `non_parse_speedup`

| Lane | Mode | Policy | Status | Metric 1 | Metric 2 | Note |
|---|---|---|---:|---:|---:|---|
| api_exe | isel | direct | OK | 288.799000 ms | 283.623000 ms | median timing |
| api_exe | isel | ir | OK | 219.319500 ms | 213.730500 ms | median timing |
| api_exe | copy_patch | direct | OK | 327.601500 ms | 320.446500 ms | median timing |
| api_exe | copy_patch | ir | OK | 227.192500 ms | 220.147000 ms | median timing |
| api_exe | llvm | direct | OK | 348.080000 ms | 333.814000 ms | median timing |
| api_exe | llvm | ir | OK | 260.098000 ms | 233.032500 ms | median timing |
| api_jit | isel | direct | OK | 285.778000 ms | 279.771000 ms | median timing |
| api_jit | isel | ir | OK | 213.824500 ms | 207.233500 ms | median timing |
| api_jit | copy_patch | direct | OK | 323.339000 ms | 314.681000 ms | median timing |
| api_jit | copy_patch | ir | OK | 224.721500 ms | 213.935000 ms | median timing |
| api_jit | llvm | direct | OK | 342.446000 ms | 324.610000 ms | median timing |
| api_jit | llvm | ir | OK | 231.075500 ms | 216.717000 ms | median timing |
| api_e2e | isel | direct | OK | 1.010571x | 1.013768x | speedup vs baseline |
| api_e2e | isel | ir | OK | 1.025699x | 1.031351x | speedup vs baseline |
| api_e2e | copy_patch | direct | OK | 1.013183x | 1.018322x | speedup vs baseline |
| api_e2e | copy_patch | ir | OK | 1.010996x | 1.029037x | speedup vs baseline |
| api_e2e | llvm | direct | OK | 1.016452x | 1.028354x | speedup vs baseline |
| api_e2e | llvm | ir | OK | 1.125597x | 1.075285x | speedup vs baseline |
| ll_jit | isel | direct | PARTIAL | 0.184400 ms | 0.051800 ms | median timing |
| ll_jit | isel | ir | PARTIAL | 0.437100 ms | 0.100500 ms | median timing |
| ll_jit | copy_patch | direct | PARTIAL | 0.477900 ms | 0.100800 ms | median timing |
| ll_jit | copy_patch | ir | PARTIAL | 0.453200 ms | 0.095300 ms | median timing |
| ll_jit | llvm | direct | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ll_jit | llvm | ir | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ll_llvm | isel | direct | PARTIAL | 2.357166 ms | 2.208459 ms | median timing |
| ll_llvm | isel | ir | PARTIAL | 2.516793 ms | 2.370625 ms | median timing |
| ll_llvm | copy_patch | direct | PARTIAL | 2.543542 ms | 2.405500 ms | median timing |
| ll_llvm | copy_patch | ir | PARTIAL | 2.507959 ms | 2.375834 ms | median timing |
| ll_llvm | llvm | direct | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ll_llvm | llvm | ir | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ll_e2e | isel | direct | PARTIAL | 12.665857x | 43.694650x | speedup vs baseline |
| ll_e2e | isel | ir | PARTIAL | 5.306454x | 23.556306x | speedup vs baseline |
| ll_e2e | copy_patch | direct | PARTIAL | 5.377838x | 23.974444x | speedup vs baseline |
| ll_e2e | copy_patch | ir | PARTIAL | 5.202488x | 23.560968x | speedup vs baseline |
| ll_e2e | llvm | direct | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ll_e2e | llvm | ir | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ir_file | isel | direct | PARTIAL | 12.665857x | 43.694650x | speedup vs baseline |
| ir_file | isel | ir | PARTIAL | 5.306454x | 23.556306x | speedup vs baseline |
| ir_file | copy_patch | direct | PARTIAL | 5.377838x | 23.974444x | speedup vs baseline |
| ir_file | copy_patch | ir | PARTIAL | 5.202488x | 23.560968x | speedup vs baseline |
| ir_file | llvm | direct | FAILED | n/a | n/a | bench_corpus_compare_failed |
| ir_file | llvm | ir | FAILED | n/a | n/a | bench_corpus_compare_failed |

## Artifact Paths

From this snapshot:
- `/tmp/liric_bench_main_full_1771322625/matrix_rows.jsonl`
- `/tmp/liric_bench_main_full_1771322625/matrix_failures.jsonl`
- `/tmp/liric_bench_main_full_1771322625/matrix_skips.jsonl`
- `/tmp/liric_bench_main_full_1771322625/matrix_summary.json`
