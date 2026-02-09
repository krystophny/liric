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
| Wall median | `10.084 ms` | `30.176 ms` | **2.99x** |
| Wall aggregate | `2803.50 ms` | `8963.53 ms` | **3.20x** |
| Materialization median (`parse+compile+lookup` vs `parse+jit+lookup`) | `0.713 ms` | `6.088 ms` | **9.14x** |
| Materialization aggregate | `267.20 ms` | `3834.94 ms` | **14.35x** |

Phase split medians:
- liric: parse `0.656 ms`, compile `0.059 ms`, lookup `0.0001 ms`
- lli: parse `0.378 ms`, jit `0.0041 ms`, lookup `5.717 ms`

Faster-case counts:
- wall: `269/270`
- materialization: `270/270`

### API JIT Mode (Primary, `bench_api_jit`)

Command:

```bash
./build/bench_api_jit --iters 3 --bench-dir /tmp/liric_bench --min-completed 1
```

Coverage: `100` attempted, `100` completed, `0` skipped.

| Metric | liric | LLVM baseline | Speedup |
|---|---:|---:|---:|
| Frontend median (shared LFortran emit) | `10.887 ms` | `10.887 ms` | `1.00x` |
| Wall median (frontend + materialization + first call) | `11.537 ms` | `18.069 ms` | **1.54x** |
| Wall aggregate | `1543 ms` | `2794 ms` | **1.81x** |
| JIT materialization median | `0.619 ms` | `6.755 ms` | **11.97x** |
| JIT materialization aggregate | `87 ms` | `1346 ms` | **15.43x** |
| Execution median (entry call only) | `0.017 ms` | `0.021 ms` | **1.22x** |

Faster-case counts:
- wall: `99/100`
- materialization: `100/100`
- execution: `80/100`

### API Object Mode (Legacy, `bench_api`)

Command:

```bash
./build/bench_api --iters 3 --bench-dir /tmp/liric_bench --min-completed 1
```

Coverage: `100` attempted, `18` completed, `82` skipped (`liric_run_failed`).

| Metric | lfortran+`WITH_LIRIC` | lfortran+LLVM | Speedup |
|---|---:|---:|---:|
| Wall median (compile+run) | `29.086 ms` | `39.514 ms` | **1.39x** |
| Wall aggregate | `522 ms` | `736 ms` | **1.41x** |
| Compile median | `28.532 ms` | `38.877 ms` | **1.39x** |
| Compile aggregate | `512 ms` | `726 ms` | **1.42x** |
| Run median | `0.541 ms` | `0.556 ms` | **1.01x** |
| Run aggregate | `10 ms` | `10 ms` | **1.03x** |

Faster-case counts (completed set only):
- wall: `18/18`
- compile: `18/18`
- run: `11/18`

Interpretation: object-mode is no longer fully blocked, but coverage is still partial (`82` run failures). Primary API KPIs remain based on `bench_api_jit`.

## License

MIT
