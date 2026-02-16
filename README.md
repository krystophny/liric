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
./build/bench_matrix --manifest tools/bench_manifest.json --modes all --policies all --lanes all --iters 1

# lane tools (bench_matrix calls these internally)
./build/bench_compat_check --timeout 15   # correctness gate
./build/bench_ll --iters 3                # liric JIT vs lli
./build/bench_lane_api --iters 3 --runtime-lib ../lfortran/build/src/runtime/liblfortran_runtime.so --liric-policy direct
                                          # lfortran LLVM vs lfortran+liric
# prerequisite for bench_corpus: populate /tmp/liric_lfortran_mass/cache
./tools/lfortran_mass/nightly_mass.sh --output-root /tmp/liric_lfortran_mass
./build/bench_corpus --iters 3            # 100-case focused corpus
./build/bench_lane_ir --iters 3 --policy direct
./build/bench_lane_micro --iters 10 --policy direct
./build/bench_exe_matrix --iters 3        # ll->exe matrix: isel/copy_patch/llvm vs clang baseline
```

Artifacts:
- `/tmp/liric_bench/matrix_summary.json`
- `/tmp/liric_bench/matrix_rows.jsonl`
- `/tmp/liric_bench/matrix_failures.jsonl`

Cell status:
- attempted: `18`
- ok: `11`
- failed: `7`

Open failing cells:
- `api_e2e + isel/direct` and `api_e2e + copy_patch/direct`: 99/100 (stale external lfortran binary, #417 liric fix merged)
- `api_e2e + llvm/direct`: 18/100 (#418)
- `ir_file + llvm/direct`: 3/100 (#415)
- `micro_c` with `policy=ir` fails in all 3 backend modes, SIGSEGV (#416)

### Non-Parse Speedup vs LLVM (`ir_file` lane)

Definition here:
- Liric non-parse = feed/materialization + lookup (parse removed)
- LLVM non-parse = `add_module + lookup`

| Mode | Policy | Liric non-parse median (ms) | LLVM non-parse median (ms) | Speedup vs LLVM |
|---|---|---:|---:|---:|
| isel | direct | 0.165217 | 4.624600 | 31.705910x |
| isel | ir | 0.160227 | 4.736892 | 29.238326x |
| copy_patch | direct | 0.154046 | 4.404326 | 32.148272x |
| copy_patch | ir | 0.160643 | 4.679654 | 29.725498x |
| llvm | ir | 5.320250 | 4.366555 | 0.816893x |
| llvm | direct | partial | partial | partial |

### Non-Parse Speedup vs LLVM (`api_e2e` lane)

Definition here:
- backend-tunable non-parse = `llvm_to_jit + run` from phase timing

| Mode | Policy | Liric non-parse median (ms) | LLVM non-parse median (ms) | Speedup vs LLVM |
|---|---|---:|---:|---:|
| isel | direct | 0.724500 | 9.844000 | 13.587302x |
| isel | ir | 0.697000 | 10.191500 | 14.621951x |
| copy_patch | direct | 0.784500 | 10.126500 | 12.908222x |
| copy_patch | ir | 0.707500 | 10.378000 | 14.668551x |
| llvm | direct | 11.026500 | 9.777500 | 0.886727x |
| llvm | ir | 10.545500 | 10.147500 | 0.962259x |

### TCC Baseline (`micro_c` lane)

`micro_c` compares Liric against TCC (not LLVM):
- `isel/direct` non-parse speedup vs TCC: `6.217420x`
- `copy_patch/direct` non-parse speedup vs TCC: `5.670741x`
- `llvm/direct` non-parse speedup vs TCC: `0.130222x`

## Source Layout

- Core: `src/`
- Public headers: `include/liric/`
- LLVM-compat headers: `include/llvm/`, `include/llvm-c/`
- Tools/benchmarks: `tools/`
- Tests: `tests/`

## License

MIT
