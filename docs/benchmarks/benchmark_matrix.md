# Unified Benchmark Matrix (`bench_matrix`)

`bench_matrix` is the canonical benchmark entrypoint.

It replaces ad-hoc manual sequencing with one strict matrix run across:
- Modes: `isel`, `copy_patch`, `llvm`
- Lanes:
  - `ir_file`: corpus-based LIRIC vs real LLVM baseline (`bench_corpus_compare` + `bench_lli_phases`)
  - `api_e2e`: LFortran `--jit` backend comparison (`bench_api`)
  - `micro_c`: TinyCC baseline lane (`bench_tcc`)

## Hard-fail policy

Default policy is strict:
- Any lane/mode failure fails the full run.
- Empty or partial datasets fail the lane.
- Missing required tools/binaries fail the lane.

Use `--allow-partial` only for exploratory local runs.

## Command

```bash
./build/bench_matrix \
  --manifest tools/bench_manifest.json \
  --bench-dir /tmp/liric_bench \
  --modes all \
  --lanes all \
  --iters 1
```

## Outputs

`bench_matrix` writes canonical artifacts under `--bench-dir`:
- `matrix_rows.jsonl`: one row per mode/lane cell
- `matrix_failures.jsonl`: one row per failing cell
- `matrix_summary.json`: run-level status and accounting

Lane-local artifacts remain in mode/lane subdirectories (for drill-down), for example:
- `/tmp/liric_bench/isel/ir_file/bench_corpus_compare_summary.json`
- `/tmp/liric_bench/llvm/api_e2e/bench_api_summary.json`
- `/tmp/liric_bench/copy_patch/micro_c/bench_tcc_summary.json`

## Baseline meaning

- `ir_file` lane baseline is actual LLVM (`lli` + ORC phase timing), not LIRIC compat headers.
- `api_e2e` lane baseline is actual LFortran LLVM backend.
- `micro_c` lane baseline is TinyCC (TCC), mandatory for this lane.

## Notes

- `api_e2e` lane regenerates compatibility artifacts via `bench_compat_check` unless `--skip-compat-check` is passed.
- `bench_tcc` now emits `bench_tcc_summary.json` so micro lane results can be gated in the unified matrix.
