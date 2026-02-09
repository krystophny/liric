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
./build/liric --emit-obj out.o file.ll
./build/liric --jit file.wasm --func add --args 2 3
```

For programmatic IR construction, use the C API in `include/liric/liric.h`.

`--emit-obj` is an explicit opt-in compatibility mode. The primary/first-class path is direct JIT for low-latency compilation.

## Benchmarks

```bash
# 1) Compatibility sweep (defines which tests both liric and lli get right)
./build/bench_compat_check --timeout 15

# 2) LL benchmark: liric JIT vs lli -O0
./build/bench_ll --iters 3

# 3) LFortran backend benchmark: lfortran+liric vs lfortran+LLVM
# Uses frozen compat_api_100.* artifacts when present.
./build/bench_api --iters 3 --min-completed 100
```

Artifacts go to `/tmp/liric_bench/`.

Strict-mode artifacts:

- `compat_api_100.txt` + `compat_api_100_options.jsonl` (frozen corpus from step 1)
- `bench_api.jsonl` (per-test status, including machine-readable skip reasons)
- `bench_api_summary.json` (attempted/completed/skipped + skip reason buckets)

## Compatibility

2266 lfortran integration tests, compared against `lfortran --backend=llvm`.

| | Count | % |
|---|---:|---:|
| LLVM produces correct output | 2246 | 99.1 |
| liric matches LLVM output | 2049 | 90.4 |
| lli matches LLVM output | 2165 | 95.5 |
| Both liric and lli match | 2004 | 88.4 |

## Performance (LL and API Modes)

Snapshot date: `2026-02-09`

### LL Mode (`bench_ll`)

Command:

```bash
./build/bench_ll --iters 3 --bench-dir /tmp/liric_bench_100_ll
```

Coverage: `94` completed tests (`compat_ll.txt`), `3` iterations each.

| Metric (median) | liric | Reference | Speedup |
|---|---:|---:|---:|
| Wall clock (`liric_probe_runner` vs `lli -O0`) | `10.065 ms` | `20.120 ms` | **2.00x** |
| Internal materialization (`parse+compile+lookup` vs `parse+jit+lookup`) | `0.486 ms` | `4.819 ms` | **10.10x** |

Phase split medians:

- liric: parse `0.439 ms`, compile `0.051 ms`, lookup `0.0001 ms`
- lli: parse `0.302 ms`, jit `0.003 ms`, lookup `4.466 ms`

### API Mode (`bench_api`)

Command:

```bash
./build/bench_api --iters 3 --bench-dir /tmp/liric_bench_100_api \
  --compat-list /tmp/liric_bench_100_api/compat_api.txt \
  --options-jsonl /tmp/liric_bench_100_api/compat_api_options.jsonl
```

Coverage: `100` attempted, `15` completed, `85` skipped.

| Metric (median, completed tests only) | lfortran+liric | lfortran+LLVM | Speedup |
|---|---:|---:|---:|
| Wall clock | `28.270 ms` | `38.655 ms` | **1.36x** |
| Compile phase | `27.770 ms` | `38.066 ms` | **1.36x** |
| Run phase | `0.537 ms` | `0.597 ms` | **1.09x** |

Faster-case counts on completed tests:

- Wall: `15/15`
- Compile: `15/15`
- Run: `14/15`

API-mode medians are directional until completion rises above `15/100`.

## License

MIT
