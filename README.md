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
| liric matches LLVM output | 2051 | 90.5 |
| lli matches LLVM output | 2166 | 95.6 |
| Both liric and lli match | 2006 | 88.5 |

## LL Benchmark: liric vs lli

2006 tests (the "both match" set), 3 iterations, CachyOS x86_64, February 2026.

**Wall-clock** (full subprocess):

| | Median | Aggregate | Faster |
|---|---:|---:|---:|
| liric | 10.1 ms | 20.5 s | 99.2% |
| lli | 20.1 ms | 48.2 s | |
| Speedup | 2.0x | 2.4x | |

**Compile time** (in-process: parse + compile + symbol resolve):

| | Median | Aggregate | Faster |
|---|---:|---:|---:|
| liric | 0.82 ms | 2.6 s | 100% |
| lli | 5.15 ms | 18.5 s | |
| Speedup | 6.4x | 7.0x | |

## License

MIT
