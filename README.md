# Liric

JIT compiler for LLVM IR (`.ll`) and WebAssembly (`.wasm`). C11, zero dependencies.

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

## Benchmarks

```bash
# 1) Compatibility sweep (defines which tests both liric and lli get right)
./build/bench_compat_check --timeout 15

# 2) LL benchmark: liric JIT vs lli -O0
./build/bench_ll --iters 3

# 3) LFortran backend benchmark: lfortran+liric vs lfortran+LLVM
./build/bench_api --iters 3
```

Artifacts go to `/tmp/liric_bench/`.

## Compatibility

2266 lfortran integration tests, compared against `lfortran --backend=llvm`.

| | Count | % |
|---|---:|---:|
| LLVM produces correct output | 2246 | 99.1 |
| liric matches LLVM output | 2049 | 90.4 |
| lli matches LLVM output | 2165 | 95.5 |
| Both liric and lli match | 2004 | 88.4 |

## LL Benchmark: liric vs lli

1962 tests (the "both match" set), 3 iterations, CachyOS x86_64, February 2026.

**Wall-clock** (full subprocess):

| | Median | Aggregate | Faster |
|---|---:|---:|---:|
| liric | 10.1 ms | 20.5 s | 98.8% |
| lli | 20.1 ms | 48.3 s | |
| Speedup | 2.0x | 2.4x | |

**Compile time** (in-process: parse + compile + symbol resolve):

| | Median | Aggregate | Faster |
|---|---:|---:|---:|
| liric | 0.80 ms | 2.6 s | 100% |
| lli | 5.13 ms | 18.8 s | |
| Speedup | 6.6x | 7.2x | |

## API Benchmark: lfortran+liric vs lfortran+LLVM

159 tests (where both backends produce correct output), 1 iteration, CachyOS x86_64, February 2026.

Wall-clock covers the full pipeline: compile + link/JIT + run.

| | Median | Aggregate | Faster |
|---|---:|---:|---:|
| lfortran+liric | 40.2 ms | 6.5 s | 98.7% |
| lfortran+LLVM | 60.3 ms | 9.0 s | |
| Speedup | 1.5x | 1.4x | |

## License

MIT
