# Unified Benchmark Matrix (`bench_matrix`)

`bench_matrix` is the canonical benchmark entrypoint.

It runs a strict matrix across:
- Modes: `isel`, `copy_patch`, `llvm`
- Policies: `direct`, `ir`
- Lanes:
  - `api_full_llvm`, `api_full_liric`
  - `api_backend_llvm`, `api_backend_liric`
  - `ll_jit`, `ll_llvm`
  - `micro_c`

Lane availability is capability-aware:
- LL and micro lanes are skipped for `mode=llvm`.
- API lanes in `mode=llvm` require `WITH_REAL_LLVM_BACKEND=ON`; otherwise they are skipped.

## Hard-Fail Policy

Default behavior is strict:
- Any lane/mode failure fails the run.
- Empty/missing datasets fail the lane.
- Missing required binaries fail the lane.

Use `--allow-partial` only for exploratory local runs.

## Command

```bash
./build/bench_matrix \
  --manifest tools/bench_manifest.json \
  --bench-dir /tmp/liric_bench \
  --modes all \
  --policies all \
  --lanes all
```

## Outputs

`bench_matrix` writes:
- `matrix_rows.jsonl`
- `matrix_failures.jsonl`
- `matrix_summary.json`

Provider artifacts remain in per-cell bundles:
- API provider: `<bench_dir>/<mode>/<policy>/api_bundle/bench_api_summary.json`
- LL provider: `<bench_dir>/<mode>/<policy>/ll_bundle/bench_corpus_compare_summary.json`
- micro provider: `<bench_dir>/<mode>/<policy>/micro_bundle/bench_tcc_summary.json`

## Baseline Meaning

- API lanes baseline: LFortran LLVM build
- LL lanes baseline: LLVM (`lli`)
- micro lane baseline: TinyCC (TCC)

## Notes

- API lanes run strict LFortran rebuild preflight by default; disable only with `--skip-lfortran-rebuild`.
- API lanes regenerate compatibility artifacts via `bench_compat_check` unless `--skip-compat-check` is set.
