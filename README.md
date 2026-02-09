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

## Performance Metrics (API First)

Primary KPIs for API-heavy workloads:

1. API wall-clock (`lfortran+liric` vs `lfortran+LLVM`)
2. Compile phase time (`compile+link` vs `compile+JIT`)
3. Run phase time (first execution)

`bench_api` now uses blocking `waitpid` + `SIGALRM` timeout handling (no 10ms polling quantization).

### 100-case API Mode Snapshot (2026-02-09)

Command:

```bash
./build/bench_api --iters 3 --bench-dir /tmp/liric_bench_100_api
```

Attempted 100 tests, completed 15 on this machine (others skipped due runtime errors).
Medians on completed tests:

- Wall: `liric 28.270 ms` vs `llvm 38.655 ms` -> **1.36x faster**
- Compile: `liric 27.770 ms` vs `llvm 38.066 ms` -> **1.36x faster**
- Run: `liric 0.537 ms` vs `llvm 0.597 ms` -> **1.09x faster**

### 100-case LL Mode Snapshot (2026-02-09)

Command:

```bash
./build/bench_ll --iters 3 --bench-dir /tmp/liric_bench_100_ll
```

Attempted 100 tests, completed 94.
Medians:

- Wall (`liric_probe_runner` vs `lli`): `liric 10.065 ms` vs `lli 20.120 ms` -> **2.00x faster**
- Internal materialization (fair split): `liric 0.486 ms` vs `lli 4.819 ms` -> **10.10x faster**
- Liric split: parse `0.439 ms`, compile `0.051 ms`, lookup `0.0001 ms`

### Secondary Diagnostics (Lexer/Parser)

Keep these separate from primary API KPIs:

- LL parse median (`bench_ll`, 100-case): `0.439 ms`
- Corpus parse share (`bench_corpus`): `~84%` of JIT time
- Lexer hotspot share (callgrind): `lr_lexer_next ~46%`
- Parser hotspots (callgrind): `parse_function_def ~4%`, `parse_type ~3%`

## License

MIT
