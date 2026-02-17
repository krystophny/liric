# Liric

Liric is a C11 JIT compiler for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

This repository uses one C benchmark driver with an explicit lane/mode/policy matrix.

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

### Primitive Lanes

- `api_exe`: API lane baseline side (`lfortran LLVM`) from `bench_api`
- `api_jit`: API lane liric side (`lfortran WITH_LIRIC`) from `bench_api`
- `ll_jit`: LL lane liric side from `bench_corpus_compare`
- `ll_llvm`: LL lane LLVM side from `bench_corpus_compare`
- `micro_c`: tiny C micro lane from `bench_tcc`

### Derived Lanes (Compatibility Aliases)

- `api_e2e`: derived speedup view from `api_exe` vs `api_jit`
- `ll_e2e`: derived speedup view from `ll_llvm` vs `ll_jit`
- `ir_file`: compatibility alias of `ll_e2e`

`api_e2e` is retained for compatibility with existing automation.

### Matrix Axes

- `modes`: `isel`, `copy_patch`, `llvm`
- `policies`: `direct`, `ir`
- `lanes`: any subset of primitive and derived lanes above

Full matrix = `lane x mode x policy`.

## Artifacts

`/tmp/liric_bench/`:
- `matrix_rows.jsonl`: one row per attempted matrix cell
- `matrix_failures.jsonl`: failed cells with failure reason
- `matrix_skips.jsonl`: failed cells with normalized skip category + fix contract
- `matrix_summary.json`: global matrix accounting and skip category totals

## Notes

- Benchmarks are `-O0`.
- `--file-skip-issues --github-repo OWNER/REPO` enables auto filing of one GitHub issue per skip category.
