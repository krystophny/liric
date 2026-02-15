# LFortran Mass Testing for Liric

## Goal
Run nightly compatibility gating without a required Python path by using
integration metadata from `../lfortran/integration_tests/CMakeLists.txt` and
the C benchmark tools (`bench_compat_check` + shell post-processing).

The nightly workflow path is shell + jq + C tooling only (no Python dependency).

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
- `summary.json`: machine-readable aggregate metrics
- `summary.md`: aggregate metrics
- `unsupported_bucket_coverage.md`: unsupported taxonomy buckets mapped to issue/rationale ownership
- `failures.csv`: non-pass rows for triage
- `bench/compat_check.jsonl`: compatibility rows from `bench_compat_check`

## Statistics in Summary
`summary.md` reports:
- Total selected tests
- LLVM emission attempted/succeeded
- Liric parse attempted/passed
- Liric JIT attempted/passed
- Differential attempted/exact-match counts
- Classification counts
- Unsupported taxonomy counts per root-cause node (`stage|symptom|feature_family`)
- Unsupported bucket-to-issue coverage (or explicit deferred rationale)
- Unsupported trend vs baseline (`improving`/`regressing`/`stable`)
- Mismatch count
- New supported regressions vs baseline
- Gate decision

## Selection Rules
- `integration_tests/CMakeLists.txt` entries are included only if `RUN(...)` has
  native `llvm` label.
- `RUN(... FAIL ...)` integration tests are excluded by default.

## Benchmarking

`bench_matrix` is the canonical benchmark entrypoint. It runs all modes and lanes
with strict accounting and one output schema.

### Workflow

1. **Unified matrix run (canonical)**:
   ```bash
   ./build/bench_matrix \
     --manifest tools/bench_manifest.json \
     --bench-dir /tmp/liric_bench \
     --modes all \
     --lanes all \
     --iters 1
   ```
   Lanes:
   - `ir_file`: LIRIC vs real LLVM baseline (`bench_corpus_compare`/`bench_lli_phases`)
   - `api_e2e`: lfortran LLVM backend vs lfortran WITH_LIRIC (`bench_api`)
   - `micro_c`: TinyCC baseline (`bench_tcc`)

   Canonical outputs:
   - `/tmp/liric_bench/matrix_rows.jsonl`
   - `/tmp/liric_bench/matrix_failures.jsonl`
   - `/tmp/liric_bench/matrix_summary.json`

2. **Compatibility check (lane artifact generator; invoked by matrix for API lane by default):**
   ```bash
   ./build/bench_compat_check --timeout 15
   ```
   Runs each eligible integration test through lfortran LLVM native, liric JIT, and lli.
   Only tests producing identical output are included in benchmarks.
   Executed binaries run in isolated temp workdirs under `/tmp/liric_bench/`
   so benchmark runs do not leave generated files in the repo root.
   Outputs: `/tmp/liric_bench/compat_api.txt`, `/tmp/liric_bench/compat_ll.txt`

3. **LL-file benchmark** (legacy lane tool, now wrapped by matrix):
   ```bash
   ./build/bench_ll --iters 3
   ```
   Reports two metrics:
   - WALL-CLOCK: subprocess `liric_probe_runner` vs subprocess `lli -O0`
   - JIT-INTERNAL (fair): in-process `liric` parse+compile+lookup vs in-process LLVM ORC parse+jit+lookup
   Also writes split summary (`parse`, `compile/jit`, `lookup`) plus legacy
   parse+compile-vs-parse+jit speedup to
   `/tmp/liric_bench/bench_ll_summary.json`.

4. **API benchmark (legacy lane tool, now wrapped by matrix):**
   ```bash
   ./build/bench_api --iters 3 --timeout-ms 3000 --min-completed 1
   ```
   Compares full `lfortran --jit` execution through:
   - LLVM backend build
   - WITH_LIRIC build
   Outputs include completion/skip accounting and per-phase medians in
   `/tmp/liric_bench/bench_api_summary.json`.

   For failure deep-dive triage:
   ```bash
   ./build/bench_api --iters 1 --timeout-ms 3000 \
     --keep-fail-workdirs \
     --fail-log-dir /tmp/liric_bench/fail_logs
   ```

5. **Optional per-file LLVM in-process phase timing**:
   ```bash
   ./build/bench_lli_phases --json --iters 1 --sig i32_argc_argv /tmp/liric_bench/ll/<test>.ll
   ```

6. **API clean-pass gate** (required before closing API clean-pass tasks):
   ```bash
   ./tools/bench_api_clean_gate.sh
   ```
   This gate fails unless all are true:
   - `attempted == completed`
   - `skipped == 0`
   - `failed == 0`
   - `zero_skip_gate_met == true`

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
- `bench_api.jsonl`: API benchmark timing/skip rows
- `bench_api_summary.json`: API benchmark aggregate medians, skip reasons, phase tracker, and
  ownership split (`phase_split`) for:
  - LFortran-only pre-backend phases (`File reading` .. `ASR -> mod`)
  - LFortran/LLVM codegen phases (`LLVM IR creation` + `LLVM opt`)
  - Backend-tunable phases (`LLVM -> JIT` + `JIT run`)
- `bench_api_failures.jsonl`: one row per skipped API case with reason/rc/signal/excerpts/log paths
- `bench_api_fail_summary.json`: skipped-case aggregate counts + failing-side distribution
- `fail_logs/`: per-failure stdout/stderr logs referenced by `bench_api_failures.jsonl`

Callgrind hotspot summarizer for API mode:
```bash
./build/bench_api_callgrind_hot \
  --bench-jsonl /tmp/liric_bench_callgrind/bench_api.jsonl \
  --liric-dir /tmp/liric_bench_callgrind/callgrind/liric \
  --llvm-dir /tmp/liric_bench_callgrind/callgrind/llvm \
  --out /tmp/liric_bench_callgrind/bench_api_callgrind_phase_hot.json
```
