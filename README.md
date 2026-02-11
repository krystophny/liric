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

Enable direct `.bc` decoding support (requires LLVM C API libs) only when needed:

```bash
cmake -S . -B build -G Ninja -DLIRIC_ENABLE_LLVM_BITCODE=ON
```

## Run

```bash
./build/liric file.ll                  # emits ./a.out
./build/liric -o myprog file.ll        # emits ./myprog
./build/liric --dump-ir file.ll
./build/liric --emit-obj out.o file.ll
./build/liric --jit file.wasm --func add --args 2 3
```

For programmatic IR construction, use the C API in `include/liric/liric.h`.

## Benchmarks

```bash
# 1) Compatibility sweep (creates shared benchmark corpus)
./build/bench_compat_check --timeout 15

# 2) LL benchmark: liric JIT vs lli -O0
./build/bench_ll --iters 3

# 3) API benchmark (primary, direct JIT): lfortran --jit (WITH_LIRIC) vs lfortran --jit (LLVM)
./build/bench_api --iters 3 --timeout-ms 3000 --min-completed 1

# 4) Clean-pass gate (required before claiming API-mode success)
./tools/bench_api_clean_gate.sh
```

Artifacts go to `/tmp/liric_bench/`.

Primary artifacts:

- `compat_ll.txt` + `compat_ll_options.jsonl` (shared corpus used by LL and API-JIT)
- `bench_ll.jsonl` + `bench_ll_summary.json`
- `bench_api.jsonl` + `bench_api_summary.json`
- `bench_api_callgrind_phase_hot.json` (via `bench_api_callgrind_hot` when callgrind traces exist)
- `bench_api_failures.jsonl` + `bench_api_fail_summary.json`
- `fail_logs/` (per-failure stdout/stderr logs)

## Compatibility

Compatibility is computed by `bench_compat_check` and written to:
- `/tmp/liric_bench/compat_check.jsonl`
- `/tmp/liric_bench/compat_api.txt`
- `/tmp/liric_bench/compat_ll.txt`

Counts vary across revisions and environments, so this README intentionally avoids hardcoded snapshot numbers.

## Performance

Performance snapshots are generated locally and stored under `/tmp/liric_bench/`.
This README does not keep hardcoded timing tables to avoid stale results.

Get current LL summary:
```bash
./build/bench_ll --iters 3 --bench-dir /tmp/liric_bench
cat /tmp/liric_bench/bench_ll_summary.json
```

Get current API summary:
```bash
./build/bench_api --iters 3 --timeout-ms 3000 --bench-dir /tmp/liric_bench --min-completed 1
# Optional: feed profile-derived lookup/dispatch share (percent) for issue #233 tracker gating
./build/bench_api --iters 3 --timeout-ms 3000 --bench-dir /tmp/liric_bench --min-completed 1 \
  --lookup-dispatch-share-pct 0.22
cat /tmp/liric_bench/bench_api_summary.json
```

Phase ownership split (now included in `bench_api_summary.json`):
- `phase_split.lfortran_requires_changes.before_asr_to_mod`
- `phase_split.lfortran_requires_changes.codegen_llvm_ir_plus_opt`
- `phase_split.backend_tunable.llvm_to_jit_plus_run`

Callgrind hotspot summary over API traces:
```bash
./build/bench_api_callgrind_hot \
  --bench-jsonl /tmp/liric_bench_callgrind/bench_api.jsonl \
  --liric-dir /tmp/liric_bench_callgrind/callgrind/liric \
  --llvm-dir /tmp/liric_bench_callgrind/callgrind/llvm \
  --out /tmp/liric_bench_callgrind/bench_api_callgrind_phase_hot.json
```

Capture richer failure diagnostics while triaging:
```bash
./build/bench_api --iters 1 --timeout-ms 3000 --bench-dir /tmp/liric_bench \
  --keep-fail-workdirs --fail-log-dir /tmp/liric_bench/fail_logs
cat /tmp/liric_bench/bench_api_fail_summary.json
```

Definition of done for "API mode is fixed":
```bash
./tools/bench_api_clean_gate.sh
```

This command fails unless:
- `skipped == 0`
- `attempted == completed`
- `failed == 0`
- `zero_skip_gate_met == true`

## License

MIT
