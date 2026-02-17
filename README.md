# Liric

Liric is a low-latency JIT compiler and native emitter for LLVM IR text (`.ll`), LLVM bitcode (`.bc`), and WebAssembly (`.wasm`).

- Language: C11
- Dependencies: none (core)
- Targets: x86_64, aarch64, riscv64 (host JIT)
- Output: JIT, object (`.o`), executable

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Testing policy (strict, enforced by CTest):
- Every test runs in an isolated work directory under `build/ctest_work/`.
- Source-tree artifact leaks (`*.mod`, `*.smod`, `_lfortran_generated_file_*`, etc.) are purged before tests and fail the run if found after tests.

Optional flags:
- `-DWITH_LLVM_COMPAT=ON`: build C++ LLVM-compat headers/tests
- `-DWITH_REAL_LLVM_BACKEND=ON`: enable real LLVM backend mode

## Quick CLI Usage

```bash
# executable (default)
./build/liric input.ll
./build/liric -o prog input.ll

# object (if no @main)
./build/liric -o out.o module_without_main.ll

# JIT
./build/liric --jit input.ll
./build/liric --jit input.ll --func main
```

Input format is auto-detected (`.wasm` magic, `.bc` magic, else `.ll`).

## Modes And Policies

Two independent controls exist:

1. Compile backend (`LIRIC_COMPILE_MODE`):
- `isel` (default): native instruction selector backend
- `copy_patch`: template copy-and-patch backend
- `llvm`: real LLVM backend

2. Session policy (`LIRIC_POLICY` or API config):
- `direct` (default): streaming path
- `ir`: explicit IR materialization path

Design intent: no hidden fallback between public modes. Unsupported operations should fail clearly.

## Unified Public API

Use `include/liric/liric.h` as the primary API (`lr_compiler_*`).

- `lr_compiler_create()`
- `lr_compiler_feed_ll()` / `lr_compiler_feed_bc()` / `lr_compiler_feed_wasm()` / `lr_compiler_feed_auto()`
- `lr_compiler_lookup()`
- `lr_compiler_emit_object()` / `lr_compiler_emit_exe()`

LLVM compatibility layers are built on top (`include/llvm/**`, `include/llvm-c/**`).
Liric intentionally does **not** export exact LLVM C symbol names to avoid linker collisions.

## Architecture Flowchart

```mermaid
flowchart LR
    A[CLI: liric] --> U
    B[C API: lr_compiler_*] --> U
    C[LLVM C++ headers] --> D[LLVM-compatible C shim\nLLVMLiric* / lc_*] --> U

    E[File input: .ll / .bc / .wasm] --> F[Frontend registry\nLL parser / BC decoder / WASM decoder]
    F --> U

    U[Unified compiler/session layer] --> P{Policy}
    P -->|direct (default)| G[Direct streaming path]
    P -->|ir| H[IR path\nmaterialize lr_module_t + finalize]

    G --> M{Backend mode}
    H --> M

    M --> I[ISel]
    M --> J[Copy-and-patch]
    M --> K[Real LLVM backend]

    I --> O[Outputs: JIT / .o / exe]
    J --> O
    K --> O
```

## Benchmark Workflow

Canonical run (all backends, both policies, all lanes):

```bash
./build/bench_matrix --modes all --policies all --lanes all --iters 1
```

`bench_matrix` enforces LFortran rebuild preflight for `api_e2e` by default (`../lfortran/build`, plus split WITH_LIRIC build dirs when configured). Use `--skip-lfortran-rebuild` only for controlled local debugging.

Published benchmark artifacts:
- `docs/benchmarks/readme_perf_snapshot.json`
- `docs/benchmarks/readme_perf_table.md`

Regenerate published snapshot/table:

```bash
./tools/bench_readme_perf_snapshot.sh --build-dir ./build --bench-dir /tmp/liric_bench --out-dir docs/benchmarks
```

Important defaults:
- API lane (`api_e2e`) defaults to `100` tests per cell (`--api-cases 100`)
- IR corpus lane (`ir_file`) uses `tools/corpus_100.tsv` (100 tests)
- Micro C lane (`micro_c`) uses 5 built-in cases

Lane tools:

```bash
./build/bench_compat_check --timeout 15
./build/bench_lane_ir --iters 1 --policy direct
./build/bench_lane_api --iters 1 --liric-policy direct
./build/bench_lane_micro --iters 10 --policy direct
# README perf gate expects this legacy command string to remain documented:
./build/bench_corpus_compare --iters 3
```

## Latest Matrix Snapshot (2026-02-17)

Command used:

```bash
# canonical: all modes + all lanes + strict matrix accounting
./build/bench_matrix --manifest tools/bench_manifest.json --modes all --policies all --lanes all --iters 1 --skip-compat-check --lfortran ../lfortran/build/src/bin/lfortran

# lane tools (bench_matrix calls these internally)
./build/bench_lane_ir --iters 1 --policy direct
./build/bench_lane_api --iters 1 --runtime-lib ../lfortran/build/src/runtime/liblfortran_runtime.so --liric-policy direct
./build/bench_lane_micro --iters 1 --policy direct
```

Artifacts:
- `/tmp/liric_bench/matrix_summary.json`
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`

Cell status:
- attempted: `18`
- ok: `18`
- failed: `0`

Open failing cells: none.

### Non-Parse Speedup vs LLVM (`ir_file` lane)

Definition here:
- Liric non-parse = feed/materialization + lookup (parse removed)
- LLVM non-parse = `add_module + lookup`

| Mode | Policy | Liric non-parse median (ms) | LLVM non-parse median (ms) | Speedup vs LLVM |
|---|---|---:|---:|---:|
| isel | direct | 0.127370 | 4.338790 | 36.932586x |
| isel | ir | 0.151706 | 4.243712 | 30.191785x |
| copy_patch | direct | 0.154921 | 4.484761 | 31.715055x |
| copy_patch | ir | 0.141171 | 4.265868 | 31.161581x |
| llvm | direct | 5.115333 | 4.027665 | 0.808756x |
| llvm | ir | 4.980485 | 4.215593 | 0.824343x |

### Non-Parse Speedup vs LLVM (`api_e2e` lane)

Definition here:
- backend-tunable non-parse = `llvm_to_jit + run` from phase timing

| Mode | Policy | Liric non-parse median (ms) | LLVM non-parse median (ms) | Speedup vs LLVM |
|---|---|---:|---:|---:|
| isel | direct | 9.283500 | 9.718000 | 1.046803x |
| isel | ir | 9.310000 | 9.301000 | 0.999033x |
| copy_patch | direct | 9.459000 | 9.748000 | 1.030553x |
| copy_patch | ir | 9.073500 | 9.112000 | 1.004243x |
| llvm | direct | 9.401000 | 9.435500 | 1.003670x |
| llvm | ir | 9.193000 | 9.335500 | 1.015501x |

### TCC Baseline (`micro_c` lane)

`micro_c` compares Liric against TCC (not LLVM):

| Mode | Policy | Wall-clock speedup vs TCC | Non-parse speedup vs TCC |
|---|---|---:|---:|
| isel | direct | 0.324635x | 5.537401x |
| isel | ir | 0.292722x | 6.028490x |
| copy_patch | direct | 0.298227x | 5.200318x |
| copy_patch | ir | 0.308859x | 5.729668x |
| llvm | direct | 0.186017x | 0.158901x |
| llvm | ir | 0.181531x | 0.159429x |

## Source Layout

- Core: `src/`
- Public headers: `include/liric/`
- LLVM-compat headers: `include/llvm/`, `include/llvm-c/`
- Tools/benchmarks: `tools/`
- Tests: `tests/`

## License

MIT
