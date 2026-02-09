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

## API-First Performance Metrics

If your real workload uses the API (not `.ll` parsing), track these 3 metrics first:

1. **API build time**  
Time spent in `lc_*` / builder calls to construct IR.

2. **JIT materialization time**  
Time in `lr_jit_add_module()` until function pointers are ready.

3. **Time-to-first-result**  
`API build time + JIT materialization time + first function call`.

### Why these metrics

- They map directly to API user latency.
- Parser/lexer numbers are secondary diagnostics for API-heavy usage.
- `bench_api` wall-clock is coarse-grained (10ms polling), so use it for broad trends, not micro-latency.

### Simple Baseline vs `lli` (reference)

From `./build/bench_ll --iters 1` (1963 tests, February 2026):

- Wall-clock median: `liric 10.064 ms` vs `lli 20.119 ms` -> **2.00x faster**
- JIT materialization median (fair internal): `liric 0.528 ms` vs `lli 5.100 ms` -> **9.88x faster**
- Liric internal split median: parse `0.476 ms`, compile `0.054 ms`, lookup `0.0001 ms`

Use this as a sanity baseline. For API product work, optimize and report the 3 API-first metrics above.

### Secondary Diagnostics (Lexer/Parser, Not First-Class)

Keep lexer/parser metrics separate and tracked, but treat them as second-order for API-first workloads.

- LL parse median (`bench_ll` internal split): `0.476 ms`
- Corpus parse share (`bench_corpus`): `~84%` of JIT time
- Lexer hotspot share (callgrind profile): `lr_lexer_next ~46%`
- Parser hotspot shares (callgrind profile): `parse_function_def ~4%`, `parse_type ~3%`

Use these to detect frontend regressions and guide LL-text ingestion work, not as primary API latency KPIs.

## License

MIT
