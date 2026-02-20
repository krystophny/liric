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
                              ~67-76x  ~67-76x  optional
                              LL speed LL speed baseline
                              (latest) (latest) mode

                              Lanes:
                              api_full_liric
                              api_backend_liric

  STANDALONE LANES (no lfortran involved)

  LL corpus    compat-derived corpus (latest: 31 .ll files)
               ll_jit:  liric compile median    ~0.031-0.033ms
               ll_llvm: lli compile median      ~2.20-2.30ms
               Speedup: ~67-76x (isel/copy_patch)

  Micro C      same corpus, lfortran --show-c output, liric vs TCC
               optional lane (requires `bench_tcc` / libtcc)
```

### Matrix axes

Default matrix run executes **24 cells**:
`2 modes (isel, copy_patch) x 2 policies x 6 default lanes`.

- Default command: `./build/bench_matrix --timeout 15`
- Default modes: `isel,copy_patch` (`llvm` is opt-in via `--modes all` or `--modes llvm`)
- Default lanes: `api_full_llvm,api_full_liric,api_backend_llvm,api_backend_liric,ll_jit,ll_llvm`
- `micro_c` is optional (`--lanes all` or explicit `micro_c`) and requires `bench_tcc`/libtcc. If unavailable, `micro_c` cells are skipped.

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
./build/bench_matrix --timeout 15
```

Full matrix (includes optional lanes/modes):

```bash
./build/bench_matrix --modes all --policies all --lanes all --timeout 15
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
./build/bench_corpus_compare --policy direct
./build/bench_api --liric-policy direct
./build/bench_tcc --policy direct --corpus /tmp/liric_bench/corpus_from_compat.tsv --cache-dir /tmp/liric_bench/cache_from_compat
```

Runtime artifacts:
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`
- `/tmp/liric_bench/matrix_summary.json`

`mode=llvm` API lanes require a liric build with `WITH_REAL_LLVM_BACKEND=ON`.

## Compatibility Guardrails

- Compatibility closure must stay clean in direct policy (`--policy direct`, `--liric-policy direct`) without relying on IR-policy fallback for pass claims.
- Performance updates must always be published against LLVM baseline (LFortran LLVM/API and LLVM materialization metrics).
- When performance-relevant changes land, regenerate and commit:
  - `docs/benchmarks/readme_perf_snapshot.json`
  - `docs/benchmarks/readme_perf_table.md`
  - using `./tools/bench_readme_perf_snapshot.sh --build-dir ./build --bench-dir /tmp/liric_bench --out-dir docs/benchmarks`

## Speedup Tables (2026-02-20)

Source: `/tmp/liric_bench/matrix_rows.jsonl` and `/tmp/liric_bench/matrix_summary.json` from
`./build/bench_matrix --timeout 15` (default modes: isel, copy_patch; default policies: direct, ir).

Matrix summary: `status=OK`, `cells_attempted=24`, `cells_ok=24`, `cells_failed=0`.
(`micro_c` skipped because `bench_tcc` is unavailable in this environment.)

### API AOT (lfortran + liric vs lfortran + LLVM, compat corpus)

| Mode | Policy | Pass rate | Wall speedup | Backend speedup |
|------|--------|----------:|-------------:|----------------:|
| isel | direct | 22/31 (9 skipped) | **0.97x** | **0.97x** |
| isel | ir | 25/31 (6 skipped) | **0.55x** | **0.55x** |
| copy_patch | direct | 22/31 (9 skipped) | **0.55x** | **0.55x** |
| copy_patch | ir | 25/31 (6 skipped) | **0.55x** | **0.55x** |

### LL Corpus (compile-only, compat corpus: 30/31 completed)

| Mode | Policy | LLVM (ms) | liric (ms) | Speedup |
|------|--------|----------:|-----------:|--------:|
| isel | direct | 2.303 | 0.031 | **76x** |
| isel | ir | 2.228 | 0.031 | **72x** |
| copy_patch | direct | 2.218 | 0.033 | **67x** |
| copy_patch | ir | 2.197 | 0.031 | **71x** |
