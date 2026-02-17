# Liric

Liric is a C11 compiler/JIT for LLVM IR (`.ll`) and WebAssembly (`.wasm`).

This repository uses one canonical benchmark driver: `bench_matrix`.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Architecture

```
                    Fortran source (.f90)
                  100 lfortran integration tests
                             |
                             v
              +---------------------------------+
              |  LFORTRAN FRONTEND              |
              |  parse -> ASR -> LLVM IR codegen|
              +--------+---------------+--------+
                       |               |
          +------------v---+     +-----v--------------+
          | lfortran native|     | lfortran + liric   |
          | LLVM backend   |     | (--jit, compat     |
          | (--backend llvm|     |  layer)             |
          +------------+---+     +-----+----------+---+
                       |               |          |
                       v               v          v
              +----------------+   POLICY:     POLICY:
              | LLVM ORC JIT   |   direct      ir
              | compile+link   |   |           |
              | +run           |   session     write .ll
              +----------------+   API calls   then parse
                                   func by     full module
              Lanes:               func        at once
              api_full_llvm        |           |
              api_backend_llvm     +-----+-----+
                                         |
                                +--------+--------+
                                |        |        |
                              MODE:    MODE:    MODE:
                              isel   copy_patch llvm
                                |        |        |
                              custom   copy+    serialize
                              ISel     patch    IR, pass
                              -> MIR   templates to LLVM
                              -> x86   -> x86   ORC JIT
                                |        |        |
                              ~47x     ~47x     ~1x
                              faster   faster   (baseline
                              (LL)     (LL)     validation)

                              Lanes:
                              api_full_liric
                              api_backend_liric

  STANDALONE LANES (no lfortran involved)

  LL corpus    100 .ll files (same corpus as API lanes)
               ll_jit:  liric parses+compiles each .ll   ~0.22ms
               ll_llvm: lli (LLVM) runs each .ll         ~10.4ms
               Speedup: ~47x (isel/copy_patch)

  Micro C      same corpus, lfortran --show-c output, liric vs TCC
               Speedup: ~4.5x in-process (isel/copy_patch)
```

### Matrix axes

The full matrix is **3 modes x 2 policies x 7 lanes = 42 cells**.
All lanes use the same 100-case corpus (first 100 from `compat_ll.txt`).

- **Mode** (compilation backend): `isel`, `copy_patch`, `llvm`
- **Policy** (how the compat layer routes IR): `direct`, `ir`
- **Lane** (what is measured):

| Lane | Source | What it measures |
|------|--------|-----------------|
| `api_full_llvm` | lfortran `--backend llvm` | full pipeline wall/non-parse timing |
| `api_full_liric` | lfortran `--jit` (liric) | full pipeline wall/non-parse timing |
| `api_backend_llvm` | lfortran `--backend llvm` | backend-isolated timing only |
| `api_backend_liric` | lfortran `--jit` (liric) | backend-isolated timing only |
| `ll_jit` | standalone liric | per-file compile time on .ll corpus |
| `ll_llvm` | standalone lli | per-file compile time on .ll corpus |
| `micro_c` | standalone liric vs TCC | in-process compile speed on C corpus |

API lanes run `lfortran` as a subprocess -- lfortran links against `libliric.a`,
so rebuilding lfortran is mandatory before API benchmarks.
LL and micro_c lanes are standalone (no lfortran subprocess).

## Run Matrix

```bash
./build/bench_matrix \
  --bench-dir /tmp/liric_bench \
  --modes all \
  --policies all \
  --lanes all \
  --iters 1 \
  --timeout 15 \
  --timeout-ms 5000
```

Published benchmark artifacts:
- `docs/benchmarks/readme_perf_snapshot.json`
- `docs/benchmarks/readme_perf_table.md`

Regenerate published snapshot/table:

```bash
./tools/bench_readme_perf_snapshot.sh --build-dir ./build --bench-dir /tmp/liric_bench --out-dir docs/benchmarks
```

Lane tools (bench_matrix calls these internally):

```bash
./build/bench_compat_check --timeout 15
./build/bench_corpus_compare --iters 1 --policy direct
./build/bench_api --iters 1 --liric-policy direct
./build/bench_tcc --iters 1 --policy direct --corpus /tmp/liric_bench/corpus_from_compat.tsv --cache-dir /tmp/liric_bench/cache_from_compat
```

Runtime artifacts:
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`
- `/tmp/liric_bench/matrix_summary.json`

## Speedup Tables (2026-02-18)

100-case corpus, non-parse median ms.

### API AOT (lfortran + liric AOT vs lfortran + LLVM, 100 integration tests)

| Mode | Policy | Pass rate | Wall speedup (median) | Note |
|------|--------|----------:|----------------------:|------|
| isel | direct | 33/100 | **1.4x** | linker time dominates wall clock |
| isel | ir | 37/100 | **1.4x** | linker time dominates wall clock |
| copy_patch | direct | 33/100 | **1.4x** | linker time dominates wall clock |
| copy_patch | ir | 37/100 | **1.4x** | linker time dominates wall clock |

63 cases SIGSEGV at runtime (codegen bugs in complex Fortran patterns).

### LL Corpus (compile-only, 100 .ll files, 3 iterations)

| Mode | Policy | LLVM (ms) | liric (ms) | Speedup |
|------|--------|----------:|-----------:|--------:|
| isel | direct | 10.41 | 0.220 | **47x** |
| isel | ir | 10.63 | 0.230 | **47x** |
| copy_patch | direct | 10.43 | 0.229 | **47x** |
| copy_patch | ir | 11.42 | 0.237 | **48x** |

### Micro C (liric vs TCC, corpus-driven in-process compile, 3 iterations)

| Mode | Policy | Speedup |
|------|--------|--------:|
| isel | direct | **4.3x** |
| isel | ir | **4.5x** |
| copy_patch | direct | **4.6x** |
| copy_patch | ir | **5.2x** |
