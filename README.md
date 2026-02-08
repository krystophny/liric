# Liric

Fast, minimal JIT for LLVM IR (`.ll`) and WebAssembly (`.wasm`).
C11 core, no Python in the benchmark/test harness.

```
.ll / .wasm -> parse -> SSA IR -> instruction select -> machine code -> JIT
```

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/liric --jit file.ll
./build/liric --dump-ir file.ll
./build/liric --jit file.wasm --func add --args 2 3
```

For programmatic IR construction, use the C API in `include/liric/liric.h`.

## Benchmarks (C Harness)

```bash
# 1) Compatibility sweep (defines compat sets)
./build/bench_compat_check --timeout 15

# 2) LL path benchmark (liric vs lli)
./build/bench_ll --iters 3

# 3) LFortran backend benchmark (lfortran+liric vs lfortran+LLVM)
./build/bench_api --iters 3
```

Artifacts are written to `/tmp/liric_bench/`.

## Latest Metrics

Environment: CachyOS x86_64, `lli -O0`, collected February 8, 2026 from the
latest complete artifacts.

### Compatibility (`bench_compat_check`)

| Metric | Value |
|---|---:|
| Processed tests | 2266 |
| `llvm_ok` | 2246 (99.1%) |
| `liric_match` | 1616 (71.3%) |
| `lli_match` | 2165 (95.5%) |
| `both_match` (`compat_ll`) | 1583 (69.9%) |
| liric SIGSEGV (`rc=-11`) | 170 |

### LL Benchmark (`bench_ll_summary.json`)

| Metric | liric | lli | Speedup |
|---|---:|---:|---:|
| Wall median | 10.065 ms | 20.120 ms | 2.00x |
| Wall aggregate | 1288.246 ms | 2525.965 ms | 1.96x |
| Materialized internal median | 0.325 ms | 3.060 ms | 9.54x |
| Benchmarked tests | 128 | 128 | - |

Fair internal metric is `parse+compile+lookup` (liric) vs
`parse+jit+lookup` (lli), not parse/jit alone.

### LFortran Backend Benchmark (`bench_api.jsonl`)

| Metric | lfortran+liric | lfortran+LLVM | Speedup |
|---|---:|---:|---:|
| Wall median | 40.244 ms | 50.303 ms | 1.25x |
| Wall aggregate | 1368.055 ms | 1830.849 ms | 1.34x |
| P90 / P95 speedup | - | - | 1.50x / 1.50x |
| Faster cases | 34/34 | - | 100% |

## License

MIT
