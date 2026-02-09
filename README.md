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

# 3) API JIT benchmark (primary): liric JIT vs LLVM JIT on identical emitted IR
./build/bench_api_jit --iters 3 --min-completed 1

# 4) API object-mode benchmark (legacy): lfortran+liric binary vs lfortran+LLVM binary
./build/bench_api --iters 3 --min-completed 1
```

Artifacts go to `/tmp/liric_bench/`.

Primary artifacts:

- `compat_api_100.txt` + `compat_api_100_options.jsonl` (frozen corpus from step 1)
- `bench_ll.jsonl` + `bench_ll_summary.json`
- `bench_api_jit.jsonl` + `bench_api_jit_summary.json`
- `bench_api.jsonl` + `bench_api_summary.json` (legacy object-mode)

## Compatibility

2266 lfortran integration tests, compared against `lfortran --backend=llvm`.

| | Count | % |
|---|---:|---:|
| LLVM produces correct output | 2246 | 99.1 |
| liric matches LLVM output | 2049 | 90.4 |
| lli matches LLVM output | 2165 | 95.5 |
| Both liric and lli match | 2004 | 88.4 |

## Performance

Snapshot date: `2026-02-09` (local run)

### LL Mode (`bench_ll`)

Command:

```bash
./build/bench_ll --iters 3 --timeout 20 --bench-dir /tmp/liric_bench
```

Coverage: `270` completed tests, `3` iterations each.

| Metric | liric | lli | Speedup |
|---|---:|---:|---:|
| Wall median | `10.064 ms` | `20.119 ms` | **2.00x** |
| Wall aggregate | `2777.65 ms` | `6936.12 ms` | **2.50x** |
| Materialization median (`parse+compile+lookup` vs `parse+jit+lookup`) | `0.518 ms` | `4.365 ms` | **8.43x** |
| Materialization aggregate | `215.79 ms` | `2873.72 ms` | **13.32x** |

Phase split medians:
- liric: parse `0.476 ms`, compile `0.045 ms`, lookup `0.0001 ms`
- lli: parse `0.252 ms`, jit `0.0029 ms`, lookup `4.070 ms`

Faster-case counts:
- wall: `267/270`
- materialization: `270/270`

### API JIT Mode (Primary, `bench_api_jit`)

Command:

```bash
./build/bench_api_jit --iters 3 --bench-dir /tmp/liric_bench --min-completed 1
```

Coverage: `100` attempted, `100` completed, `0` skipped.

| Metric | liric | LLVM baseline | Speedup |
|---|---:|---:|---:|
| Frontend median (shared LFortran emit) | `11.148 ms` | `11.148 ms` | `1.00x` |
| Wall median (frontend + materialization + first call) | `11.864 ms` | `17.595 ms` | **1.48x** |
| Wall aggregate | `1556.97 ms` | `2777.17 ms` | **1.78x** |
| JIT materialization median | `0.584 ms` | `6.392 ms` | **10.95x** |
| JIT materialization aggregate | `86.41 ms` | `1330.53 ms` | **15.40x** |
| Execution median (entry call only) | `0.017 ms` | `0.020 ms` | **1.19x** |

Faster-case counts:
- wall: `99/100`
- materialization: `100/100`
- execution: `78/100`

### API Object Mode (Legacy, `bench_api`)

The object/link/run benchmark is still available, but it is currently blocked by runtime hangs in `lfortran` `WITH_LIRIC` binaries.

Smoke command:

```bash
./build/bench_api --iters 1 --timeout 5 --bench-dir /tmp/liric_bench \
  --compat-list /tmp/liric_bench/compat_api_smoke10.txt \
  --options-jsonl /tmp/liric_bench/compat_api_smoke10_options.jsonl \
  --min-completed 0
```

Result: `10` attempted, `0` completed, `10` skipped (`liric_run_timeout`).

Interpretation: object-mode numbers are not used for current API performance KPIs; API performance tracking is based on `bench_api_jit`.

## License

MIT
