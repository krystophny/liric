# LFortran Mass Testing for Liric

## Goal
Run nightly compatibility gating without a required Python path by using
integration metadata from `../lfortran/integration_tests/CMakeLists.txt` and
the C benchmark tools (`bench_compat_check` + shell post-processing).

The Python mass harness remains available for offline deep-dive analysis, but is
no longer required for the active nightly workflow path.

Roadmap and scorecard are tracked in `docs/lfortran_mass_tracker.md`.
Stable mismatch/unsupported root-cause taxonomy is defined in
`docs/lfortran_failure_taxonomy.md`.

## Design Principles
- Reuse LFortran test configuration directly from `tests.toml`.
- Reuse LFortran integration test definitions directly from integration `RUN(...)`.
- Do not duplicate test lists or copy LFortran config files into this repository.
- Feed liric raw `--show-llvm` output from LFortran without preprocessing.
- Exclude expected-failure tests by default.
- Compile extrafiles by language:
  - Fortran extrafiles with LFortran (including `--cpp`/fixed-form handling)
  - C/C++ extrafiles with host C/C++ compiler (`CC`/`CXX`)
- Treat unsupported features/ABI as tracked non-fatal buckets.
- Fail only on mismatches and new supported regressions.

## Assumed Layout
- `liric`: current repository
- `lfortran`: sibling repository at `../lfortran`

Defaults are:
- LFortran root: `../lfortran`
- integration CMake: `../lfortran/integration_tests/CMakeLists.txt`
- LFortran binary: auto-detected:
  - `../lfortran/build/src/bin/lfortran` (preferred)
  - `../lfortran/build_clean_bison/src/bin/lfortran` (fallback)
- Runtime library preload: auto-detected when available:
  - `<detected-lfortran-bin>/../runtime/liblfortran_runtime.so`
  - env override: `$LFORTRAN_RUNTIME_LIBRARY_DIR/liblfortran_runtime.so`
- OpenMP runtime preload: auto-detected when selected tests use OpenMP
  (`--openmp` / `llvm_omp`), searching common `libgomp`/`libomp` locations
  - env override (explicit file): `$LFORTRAN_OPENMP_LIBRARY`
  - env override (directory): `$LFORTRAN_OPENMP_LIBRARY_DIR`

## Build Prerequisites
```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

The mass harness expects these binaries:
- `build/liric`
- `build/liric_probe_runner`

## Run Mass Campaign
```bash
./tools/lfortran_mass/nightly_mass.sh \
  --lfortran-root ../lfortran \
  --lfortran-bin ../lfortran/build/src/bin/lfortran \
  --probe-runner ./build/liric_probe_runner
```

Useful options:
- `--workers N`: forwarded to compatibility check tooling
- `--baseline <path>`: compare against previous results
- `--runtime-lib <path>`: explicit `liblfortran_runtime` location (otherwise auto-detected)
- `--diag-fail-logs`: accepted for compatibility (no-op in shell runner)
- `--compat-jsonl <path>`: generate artifacts from an existing compat JSONL file

## Outputs
All artifacts are written under `/tmp/liric_lfortran_mass/`:
- `manifest_tests_toml.jsonl`: selected manifest rows with `corpus` metadata
- `selection_decisions.jsonl`: per-case selection decision and skip reason
- `results.jsonl`: per-case outcomes
- `summary.md`: aggregate metrics
- `failures.csv`: non-pass rows for triage
- `bench/compat_check.jsonl`: compatibility rows from `bench_compat_check`

## Statistics in Summary
`summary.md` reports:
- Total selected tests
- LLVM emission attempted/succeeded
- Liric parse attempted/passed
- Liric JIT attempted/passed
- Differential attempted/completed/exact-match counts
- Selected/skipped counts per corpus
- Skip reason histogram and skip reasons by corpus
- Supported processed/passed counts
- Unsupported histogram
- Taxonomy counts per root-cause node (`stage|symptom|feature_family`)
- Mismatch count
- New supported regressions vs baseline
- Gate decision

## Selection Rules
- `integration_tests/CMakeLists.txt` entries are included only if `RUN(...)` has
  native `llvm` label.
- `RUN(... FAIL ...)` integration tests are excluded by default.

## Benchmarking

Standalone C tools compare liric JIT performance against LLVM on integration tests.
These do NOT depend on mass run results; they discover tests directly from CMakeLists.txt.

### Workflow

1. **Compatibility check** (required first):
   ```bash
   ./build/bench_compat_check --timeout 15
   ```
   Runs each eligible integration test through lfortran LLVM native, liric JIT, and lli.
   Only tests producing identical output are included in benchmarks.
   Executed binaries run in isolated temp workdirs under `/tmp/liric_bench/`
   so benchmark runs do not leave generated files in the repo root.
   Outputs: `/tmp/liric_bench/compat_api.txt`, `/tmp/liric_bench/compat_ll.txt`

2. **LL-file benchmark** (liric JIT vs LLVM lli):
   ```bash
   ./build/bench_ll --iters 3
   ```
   Reports two metrics:
   - WALL-CLOCK: subprocess `liric_probe_runner` vs subprocess `lli -O0`
   - JIT-INTERNAL (fair): in-process `liric` parse+compile+lookup vs in-process LLVM ORC parse+jit+lookup
   Also writes split summary (`parse`, `compile/jit`, `lookup`) plus legacy
   parse+compile-vs-parse+jit speedup to
   `/tmp/liric_bench/bench_ll_summary.json`.

3. **Optional per-file LLVM in-process phase timing**:
   ```bash
   ./build/bench_lli_phases --json --iters 1 --sig i32_argc_argv /tmp/liric_bench/ll/<test>.ll
   ```

### Test Selection for Benchmarks
- Integration tests with `llvm` label from CMakeLists.txt
- Excluded: FAIL tests, EXTRAFILES, llvm_omp, llvm2, llvm_rtlib labels
- Included with mapped flags: llvmImplicit, llvmStackArray, llvm_integer_8, llvm_nopragma

### Benchmark Outputs
All artifacts in `/tmp/liric_bench/`:
- `compat_check.jsonl`: per-test compatibility results
- `compat_api.txt`: test names passing liric compat
- `compat_ll.txt`: test names passing liric + lli compat
- `bench_ll.jsonl`: LL-file benchmark timing data
- `bench_ll_summary.json`: aggregate medians/sums including parser-vs-compile split
