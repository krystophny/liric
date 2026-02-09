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
# 1) Compatibility sweep (creates shared benchmark corpus)
./build/bench_compat_check --timeout 15

# 2) LL benchmark: liric JIT vs lli -O0
./build/bench_ll --iters 3

# 3) API benchmark (primary, direct JIT): lfortran --jit (WITH_LIRIC) vs lfortran --jit (LLVM)
./build/bench_api --iters 3 --min-completed 1
```

Artifacts go to `/tmp/liric_bench/`.

Primary artifacts:

- `compat_ll.txt` + `compat_ll_options.jsonl` (shared corpus used by LL and API-JIT)
- `bench_ll.jsonl` + `bench_ll_summary.json`
- `bench_api.jsonl` + `bench_api_summary.json`

## Compatibility

2266 lfortran integration tests, compared against `lfortran --backend=llvm`.

| | Count | % |
|---|---:|---:|
| LLVM produces correct output | 2246 | 99.1 |
| liric matches LLVM output | 2166 | 95.6 |
| lli matches LLVM output | 2166 | 95.6 |
| Both liric and lli match | 2121 | 93.6 |

## Performance

Snapshot date: `2026-02-09` (local run)

### LL Mode (`bench_ll`)

Command:

```bash
./build/bench_ll --iters 3 --timeout 20 --bench-dir /tmp/liric_bench
```

Coverage: `2121` attempted, `2081` completed, `40` skipped.

| Metric | liric | lli | Speedup |
|---|---:|---:|---:|
| Wall median | `10.065 ms` | `20.123 ms` | **2.00x** |
| Wall aggregate | `21419.49 ms` | `54075.08 ms` | **2.52x** |
| Materialization median (`parse+compile+lookup` vs `parse+jit+lookup`) | `0.620900 ms` | `5.427575 ms` | **9.05x** |
| Materialization aggregate | `1734.594 ms` | `22596.259 ms` | **13.03x** |

Phase split medians:
- liric: parse `0.556 ms`, compile `0.062 ms`, lookup `0.0001 ms`
- lli: parse `0.346 ms`, jit `0.0037 ms`, lookup `5.064 ms`

Faster-case counts:
- wall: `2067/2081`
- materialization: `2081/2081`

### API Mode (Primary, direct JIT, `bench_api`)

Command:

```bash
./build/bench_api --iters 3 --bench-dir /tmp/liric_bench --min-completed 1
```

Coverage: `2121` attempted, `2080` completed, `41` skipped.

| Metric | liric | LLVM baseline | Speedup |
|---|---:|---:|---:|
| Frontend median (shared LFortran emit) | `10.398 ms` | `10.398 ms` | `1.00x` |
| Wall median (frontend + materialization + first call) | `11.011 ms` | `16.252 ms` | **1.47x** |
| Wall aggregate | `29802 ms` | `51119 ms` | **1.72x** |
| JIT materialization median | `0.564 ms` | `5.726 ms` | **10.53x** |
| JIT materialization aggregate | `1629 ms` | `23213 ms` | **14.25x** |
| Execution median (entry call only) | `0.019 ms` | `0.021 ms` | **1.13x** |

Faster-case counts:
- wall: `2075/2080`
- materialization: `2080/2080`
- execution: `1609/2080`
- skipped: `source_missing=1`, `llvm_jit_failed=40`

## License

MIT
