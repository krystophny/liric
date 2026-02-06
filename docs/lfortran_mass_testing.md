# LFortran Mass Testing for Liric

## Goal
Use LFortran's declared test corpus from `../lfortran/tests/tests.toml` as the only
source of unit test cases, plus integration metadata from
`../lfortran/integration_tests/CMakeLists.txt`, generate LLVM IR with LFortran, and
evaluate compatibility in liric parse/JIT/runtime lanes.

Roadmap and scorecard are tracked in `docs/lfortran_mass_tracker.md`.

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
- tests.toml: `../lfortran/tests/tests.toml`
- integration CMake: `../lfortran/integration_tests/CMakeLists.txt`
- LFortran binary: `../lfortran/build_clean_bison/src/bin/lfortran`

## Build Prerequisites
```bash
cmake -S . -B build -G Ninja
cmake --build build -j32
ctest --test-dir build --output-on-failure
```

The mass harness expects these binaries:
- `build/liric_cli`
- `build/liric_probe_runner`

## Run Mass Campaign
```bash
python3 -m tools.lfortran_mass.run_mass
```

Useful options:
- `--limit N`: process first N tests only
- `--workers N`: control parallelism
- `--force`: ignore case cache and rerun
- `--baseline <path>`: compare against previous results
- `--update-baseline`: write current run as baseline snapshot
- `--skip-tests-toml`: run only integration corpus
- `--skip-integration-cmake`: run only unit corpus
- `--include-expected-fail`: include expected-failure/error-handling tests
- `--load-lib <path>`: preload runtime libraries into liric JIT (repeatable)

## Outputs
All artifacts are written under `/tmp/liric_lfortran_mass/`:
- `manifest_tests_toml.jsonl`: canonical list from `tests.toml`
- `manifest_tests_toml.jsonl` includes both selected corpora with a `corpus` field
- `selection_decisions.jsonl`: per-case selection decision and skip reason
- `results.jsonl`: per-case outcomes
- `summary.md`: aggregate metrics
- `failures.csv`: non-pass rows for triage
- `cache/<case_id>/`: case-level cache and raw LLVM output

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
- Mismatch count
- New supported regressions vs baseline
- Gate decision

## Differential Lane
For tests with `run = true` or `run_with_dbg = true`:
1. Execute reference run via LFortran.
2. Execute same emitted LLVM with `liric_probe_runner`.
3. Compare normalized stdout/stderr and return code.

If signature/ABI is unsupported by the runner, classify as `unsupported_abi`.

## Selection Rules
- `tests.toml` entries are included only if LLVM-intended:
  `pass_with_llvm`, `asr_implicit_interface_and_typing_with_llvm`,
  `run`, `run_with_dbg`, `obj`, or `bin`.
- `tests.toml` expected-failure unit tests are excluded when path is under `errors/`.
- `integration_tests/CMakeLists.txt` entries are included only if `RUN(...)` has
  native `llvm` label.
- `RUN(... FAIL ...)` integration tests are excluded by default.
