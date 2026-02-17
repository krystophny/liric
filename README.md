# Liric

Liric is a C11 JIT compiler for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

This repository now uses a single C benchmark driver with an explicit lane/mode matrix.
Old benchmark scripts were removed.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Benchmark Matrix (Canonical)

Run:

```bash
./build/bench_matrix --iters 3 --timeout 15
```

Defaults:
- Integration source: `../lfortran/integration_tests/CMakeLists.txt`
- LFortran binary: `../lfortran/build/src/bin/lfortran`
- Probe runner: `build/liric_probe_runner`
- Runtime lib: `../lfortran/build/src/runtime/liblfortran_runtime.(dylib|so)`
- Output dir: `/tmp/liric_bench`

### Lanes

- `api_exe`: `.f90 -> native executable` via lfortran, then run
- `api_jit`: `.f90 -> .ll` via `--show-llvm`, then liric JIT run
- `ll_jit`: pre-emitted `.ll`, then liric JIT run
- `ll_lli`: pre-emitted `.ll`, then `lli -O0` run

### Modes

- `wall`: end-to-end wall time for the lane
- `compile_only`: compile/codegen component only (if available)
- `run_only`: runtime component only (if available)
- `parse_only`: text parse component only (if available)
- `non_parse`: compile+run (parse excluded)

`ll_lli` only exposes `wall`.

### Derived Comparison Lanes

Derived only from measured lane primitives, never from mixed formulas:

- `api_e2e`: compare `api_exe` vs `api_jit`
  - wall speedup: `api_exe.wall / api_jit.wall`
  - non-parse speedup: `api_exe.non_parse / api_jit.non_parse`
- `ll_e2e`: compare `ll_lli` vs `ll_jit`
  - wall speedup: `ll_lli.wall / ll_jit.wall`

## Artifacts

`/tmp/liric_bench/`:
- `bench_matrix_rows.jsonl`: per-test metrics for all lanes/modes
- `compat_api.txt`: tests where `api_jit` matches `api_exe`
- `compat_ll.txt`: tests where `ll_jit` and `ll_lli` match `api_exe`
- `summary.md`: lane matrix + aggregate medians + derived lane speedups

## Notes

- Comparisons are reported on matched tests only.
- Benchmarks are `-O0` by design for direct JIT-vs-LLVM pipeline analysis.
