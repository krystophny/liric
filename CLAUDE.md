# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Is Liric

Liric (Lightweight IR Compiler) is a fast, minimal JIT compiler and native code emitter with three
frontends: LLVM IR text (.ll), LLVM bitcode (.bc), and WebAssembly binary (.wasm). Written in C11,
zero external dependencies. Designed for low-latency JIT where full LLVM overhead is unacceptable.

See [STATUS.md](STATUS.md) for current feature completion and LFortran integration progress.

## Build and Test

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

CTest enforces strict test sandboxing:
- Each test runs in an isolated working directory under `build/ctest_work/`
- Source-tree generated artifacts (`*.mod`, `*.smod`, `_lfortran_generated_file_*`, etc.) are purged before tests and treated as failures if present after tests

Optional build flags:
- `-DWITH_LLVM_COMPAT=ON`: C++ compat layer tests
- `-DWITH_REAL_LLVM_BACKEND=ON`: Real LLVM C API backend (Mode C)

Clean build takes ~110ms. No external dependencies beyond a C compiler and CMake (plus a C++17 compiler when `WITH_LLVM_COMPAT=ON`).

**MANDATORY: Rebuild lfortran after every liric change.** The `bench_api` / `api_e2e` lane
runs `lfortran --jit` as an external binary. That binary links against liric. If lfortran is
not rebuilt with the current liric, benchmark results are invalid and failures are misleading.
Always rebuild lfortran before running benchmarks:
```bash
cmake --build ../lfortran/build -j$(nproc)
```

## Language Policy

- Use C for compiler code, test harnesses, and benchmark infrastructure.
- Do not add new Python-based testing/benchmark infrastructure.
- Exception: the thin C++ compatibility layer that exposes a drop-in LLVM API replacement (`include/llvm/**`, `tests/test_llvm_compat.cpp`, and minimal glue needed for it).

## Benchmarking

Benchmarking is C-based and uses fair comparisons for both metrics.

**Step 1: Correctness check** (must run first):
```bash
./build/bench_compat_check --timeout 15
```
Discovers integration tests with `llvm` label, runs each through lfortran LLVM native,
liric JIT, and lli. Writes compatibility lists:
- `/tmp/liric_bench/compat_api.txt` (tests where liric matches LLVM)
- `/tmp/liric_bench/compat_ll.txt` (tests where liric AND lli match LLVM)

**Step 2: LL benchmark** (liric JIT vs lli):
```bash
./build/bench_ll --iters 3
```
Compares:
- WALL-CLOCK: subprocess `liric_probe_runner` vs subprocess `lli`
- JIT-INTERNAL: in-process `liric` parse+compile vs in-process `LLVM ORC` parse+compile

Fair LLVM internal phases are measured with:
```bash
./build/bench_lli_phases --json --iters 1 --sig i32_argc_argv /tmp/liric_bench/ll/<test>.ll
```

**Step 3: API benchmark (primary, direct JIT)** (`lfortran --jit` LLVM vs WITH_LIRIC):
```bash
./build/bench_api --iters 3
```
Compares the full lfortran JIT pipeline through both backends:
- `lfortran` (LLVM backend): frontend + LLVM JIT + run
- `lfortran` (liric backend, built with `-DWITH_LIRIC=ON`): frontend + liric JIT + run
No object-file/link benchmark path is included in API mode.

Uses `compat_ll.txt` and `compat_ll_options.jsonl` from Step 1.

All outputs go to `/tmp/liric_bench/`.

**Step 4: API clean-pass gate** (required before claiming API mode is fixed):
```bash
./tools/bench_api_clean_gate.sh
```
This command fails unless all are true:
- `attempted == completed`
- `skipped == 0`
- `failed == 0`
- `zero_skip_gate_met == true`

**Step 5: TCC comparison benchmark** (liric vs TinyCC, requires `tcc` and `libtcc`):
```bash
./build/bench_tcc --iters 10
```
Compares:
- WALL-CLOCK: subprocess `tcc -o exe file.c` vs `liric -o exe file.ll`
- IN-PROCESS: `tcc_compile_string() + tcc_relocate()` vs `lr_parse_ll() + lr_jit_add_module()`
Five micro-benchmarks: ret42, add, arith_chain, loop_sum, fib20.

**Focused corpus benchmark** (100 curated tests, fast iteration):
```bash
./build/bench_corpus                    # run all 100, print timing table
./build/bench_corpus --top 10           # show top 10 slowest
./build/bench_corpus --iters 3          # 3 iterations, keep best
./build/bench_corpus --csv              # CSV output for analysis
./build/bench_corpus --single <name>    # single test (for perf/callgrind)
```
The corpus (`tools/corpus_100.tsv`) contains 100 representative passing tests covering
39 categories, from 494 bytes to 2MB, totaling 9.8MB of LLVM IR. Requires
`/tmp/liric_lfortran_mass/cache/` from a prior mass test run.

Profiling workflow:
```bash
# callgrind (deterministic, best for optimization work)
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out \
  ./build/liric_probe_runner --ignore-retcode \
  --load-lib <runtime.so> --func main --sig i32_argc_argv <test.ll>
callgrind_annotate /tmp/cg.out | head -60

# perf (sampling, for large workloads)
perf record -g -F 999 -- ./build/bench_corpus
perf report --stdio --no-children --percent-limit 1
```

## Current Performance Profile (2026-02-12)

100-case corpus (9.8MB LLVM IR): **92ms JIT total** (parse 77ms/84%, compile 15ms/16%).

**TCC vs liric (bench_tcc, 5 micro-benchmarks, best-of-10):**

| Metric | TCC | liric | Ratio |
|--------|-----|-------|-------|
| Wall-clock exe-mode (us/case) | ~1300 | ~1400 | 0.96x (parity, process startup dominates) |
| In-process compile+reloc (us/case) | ~195 | ~54 | **3.6x faster** |
| In-process compile-only (us/case) | n/a (single-pass) | ~5 | -- |

**LFortran corpus (2266 integration tests):**

| Mode | Pass | Mismatch | Crash |
|------|------|----------|-------|
| ISel JIT (default) | 2191/2193 | 2 pre-existing | 0 |
| copy_patch JIT | 2191/2193 | 2 (identical) | 0 |

**Priority: parser/lexer optimization (issues #144, #145).**

## Pipeline

```
  .ll text       .bc bitcode      .wasm binary       Session / Compat API
      |               |                |                       |
      v               v                v                       |
   ll_lexer       bc_decode       wasm_decode                  |
   ll_parser                      wasm_to_ir                   |
      |               |                |                       |
      +---------------+----------------+-----------------------+
                                       |
                                  lr_module_t
                                  (SSA IR, 45 ops)
                                       |
                                  lr_func_finalize()
                                  (peephole passes)
                                       |
                   +-------------------+-------------------+
                   |                   |                   |
                 ISel           Copy-and-patch          Real LLVM
              (single-pass)    (template memcpy)      (LLVM C API)
                   |                   |                   |
                   +-------------------+-------------------+
                   |                   |                   |
                  JIT            Object file (.o)      Executable
```

Three frontends all produce `lr_module_t`. Auto-detection via `frontend_registry.c` checks
WASM magic, BC magic, then LL text fallback.

Three compilation modes selected via `LIRIC_COMPILE_MODE` env var:
- **isel** (default): Single-pass ISel + binary encoding. Full opcode coverage. All targets.
- **copy_patch**: Pre-assembled x86_64 templates, memcpy + patch sentinels. Falls back to ISel for unsupported opcodes. Non-x86_64 targets delegate to ISel.
- **llvm**: Translate `lr_module_t` to real LLVM C API via ORC JIT. Requires `-DWITH_REAL_LLVM_BACKEND=ON`.

### Session API Streaming Fast-Path

The session API (`liric_session.h`) provides a `LR_MODE_DIRECT` mode where trivial
single-block functions (integer ALU + ret, <=6 params) are compiled directly from a
temporary stack-local `lr_func_t` without materializing persistent IR. All other functions
replay through the normal `lr_module_t` -> `lr_func_finalize()` -> backend path.

## Source Map

All C source is in `src/`. Public headers in `include/liric/`. C++ compat headers in `include/llvm/`.

| Component | Key files | Role |
|-----------|-----------|------|
| **Arena allocator** | `arena.c` | Chunk-based bump allocator, all IR objects use it |
| **Core IR** | `ir.c`, `ir.h` | 45 opcodes, types, instructions, blocks, functions, modules (SSA). `lr_func_finalize()` runs peephole passes (const fold, identity elim, load forwarding, DCE). |
| **LL frontend** | `ll_lexer.c`, `ll_parser.c` | Tokenizer + recursive descent parser for .ll text. Dynamic hash tables (no hard limits). |
| **BC frontend** | `bc_decode.c` | Native LLVM bitcode parser, zero LLVM dependency. Streaming instruction callback. |
| **WASM frontend** | `wasm_decode.c`, `wasm_to_ir.c` | Binary decoder + stack-to-SSA converter. Streaming instruction callback. MVP integer subset. |
| **Frontend registry** | `frontend_registry.c` | Auto-detection: WASM magic, BC magic, LL fallback |
| **Target interface** | `target.h` | Backend vtable: `compile_func` (ISel), `compile_func_cp` (C&P). `lr_compile_mode_t` enum. |
| **Target registry** | `target_registry.c` | `lr_target_by_name()`, `lr_target_host()`, host detection |
| **x86_64 ISel** | `target_x86_64.c` | ISel + x86_64 binary encoder, System V ABI |
| **x86_64 C&P** | `target_x86_64_cp.c` | Copy-and-patch compiler with ISel fallback |
| **C&P templates** | `cp_template.h`, `platform/cp_templates_x86_64.S` | Template infrastructure + x86_64 asm templates |
| **Stencil system** | `stencil_data.h`, `stencil_runtime.c` | Nascent: 3 stencils (add_i32, sub_i64, fadd_f64), memcpy + relocation patching |
| **aarch64 ISel** | `target_aarch64.c` | ISel + aarch64 binary encoder, AAPCS64 ABI |
| **riscv64 ISel** | `target_riscv64.c` | ISel + riscv64 binary encoder |
| **Shared backend** | `target_common.c`, `target_shared.c` | Common ISel helpers across targets |
| **JIT engine** | `jit.c` | 16MB code + 16MB data buffers (mmap'd), W^X, symbol table (8192 buckets), negative miss cache, lazy materialization + materialization cache |
| **Object/exe emission** | `objfile.c`, `objfile_elf.c`, `objfile_macho.c`, `module_emit.c` | ELF64 + Mach-O writers, exe linking |
| **LLVM backend** | `llvm_backend.c` | Mode C: real LLVM C API (ORC JIT, obj/exe emission). Requires `-DWITH_REAL_LLVM_BACKEND=ON`. |
| **Intrinsic stubs** | `platform/*.S` | Hand-written assembly blobs for LLVM intrinsics (sqrt, exp, memcpy, etc.) |
| **Session API** | `session.c`, `include/liric/liric_session.h` | High-level incremental compilation. DIRECT mode (streaming fast-path) and IR mode. |
| **Public API** | `include/liric/liric.h`, `liric.c` | Parse (.ll/.bc/.wasm), module management, JIT lifecycle |
| **Compat C API** | `include/liric/liric_compat.h`, `liric_compat.c` | ~150 `lc_*` functions, handle-based builder, deferred PHI |
| **Public types** | `include/liric/liric_types.h` | Complete type definitions for C++ compat headers |
| **Shared IR types** | `include/liric/liric_ir_shared.h` | `lr_opcode_t`, `lr_operand_desc_t` shared between session API and core |
| **C++ LLVM compat** | `include/llvm/**/*.h` | Header-only C++17 wrappers mapping LLVM 21 API to liric |
| **CLI** | `tools/liric_main.c` | `--jit`, `--dump-ir`, `--func`, `-o` |
| **Tests** | `tests/test_*.c`, `tests/test_llvm_compat.cpp` | See Test Structure below |

## Architecture Details

**Core IR** (`ir.h`) is register-based SSA with explicit CFG:
- `lr_module_t` owns an arena + linked lists of functions and globals
- `lr_func_t` contains linked list of `lr_block_t`, each with linked list of `lr_inst_t`
- 45 opcodes (`LR_OP_*`): arithmetic, memory, control flow, type conversion, PHI, aggregate
- Type singletons for primitives (void, i1-i64, float, double, ptr); composite types allocated per-use
- Operands are tagged unions: vreg, immediate, block ref, global ref, null, undef
- `lr_func_finalize()` linearizes linked lists into arrays and runs peephole passes (constant folding, identity elimination, dead load removal, load-after-store forwarding, DCE -- up to 6 iterations)

**Backend** (`target.h`) supports two compilation modes per target:
- **ISel** (`compile_func`): Single-pass ISel + binary encoding, full opcode coverage
- **Copy-and-patch** (`compile_func_cp`): Template memcpy + sentinel patching on x86_64; other targets delegate to ISel
- Backend-local compile context holds code buffer, stack slots, and branch fixups
- `LR_CC_*` condition codes shared across backends for integer and FP comparisons
- Each backend uses native scratch registers (x86: RAX/RCX, aarch64: X9/X10)
- Stack-based register allocation: every vreg gets a stack slot
- PHI copies built before emission, applied at block terminators
- JIT dispatches via `lr_compile_mode_t` field; C&P auto-falls back to ISel for unsupported opcodes

**Backend interface** (`lr_target_t`):
```c
typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;
    int (*compile_func)(lr_func_t *, lr_module_t *, uint8_t *, size_t, size_t *, lr_arena_t *);
    int (*compile_func_cp)(lr_func_t *, lr_module_t *, uint8_t *, size_t, size_t *, lr_arena_t *);
} lr_target_t;
```

**JIT memory model:**
- Code buffer: 16MB mmap'd region, W^X transitions via mprotect (Linux) or MAP_JIT + pthread_jit_write_protect_np (macOS)
- Data buffer: 16MB mmap'd region for globals
- Symbol table: FNV-1a hash, 8192 buckets, negative miss cache (4096 buckets)
- Lazy materialization: functions can be registered lazy, compiled on first lookup
- Materialization cache: signature-based code reuse across invocations
- `__builtin___clear_cache` for icache coherence on arm64

**Three compilation modes** (selected via `LIRIC_COMPILE_MODE` env var):
- `isel` (default): ISel + encoding, full opcode coverage
- `copy_patch`: Pre-assembled templates, 3.6x faster than TCC. Falls back to ISel for unsupported opcodes.
- `llvm`: Translate to real LLVM C API for multi-pass optimization. Requires build flag.

**Module loading:**
- Monolithic: all functions compiled together via `lr_jit_add_module()`
- Incremental: multiple modules can be added to one JIT instance
- Runtime bitcode injection: `lr_jit_set_runtime_bc()` provides a .bc blob merged on first module add

**Four-layer programmatic API:**
```
C++ LLVM 21 headers   include/llvm/**  (header-only wrappers, drop-in replacement)
        |
C compat API          liric_compat.h   (lc_value_t handles, deferred PHI, ~150 functions)
        |
C session API         liric_session.h  (lr_session_emit(), DIRECT/IR modes)
        |
C core                ir.h, jit.h      (lr_module_t, lr_jit_t, arena allocator)
```

## Platform Support

| Platform | JIT | Obj | Exe | Notes |
|----------|-----|-----|-----|-------|
| Linux x86_64 | yes | ELF | ELF | `mprotect` W^X, `-ldl` for `dlsym` |
| Linux aarch64 | yes | ELF | ELF | AAPCS64 ABI |
| Linux riscv64 | yes | -- | ELF | gc and im variants |
| macOS aarch64 | yes | Mach-O | Mach-O | `MAP_JIT` + `pthread_jit_write_protect_np`, icache flush |

JIT is host-only: `lr_jit_create()` auto-detects, cross-target fails fast.

## Where To Change What

- **Add a new frontend:** Parse input format into `lr_module_t` using the `ir.h` API. Register in `frontend_registry.c`.
- **Add/modify a backend:** Implement `lr_target_t` vtable in `src/target_<name>.c`, register in `src/target_registry.c`
- **Add an instruction:** Add opcode to `lr_opcode_t` in `include/liric/liric_ir_shared.h`, handle in parser, ISel, and encoder
- **Change public API:** Update `include/liric/liric.h` and mirror in internal headers
- **Add tests:** Write in `tests/test_<category>.c`, register in `tests/test_main.c`, add source to `CMakeLists.txt`

## Test Structure

Tests are a single executable (`test_liric`) with categories:
- **Lexer** (3): token recognition
- **Parser** (3): IR construction from .ll text
- **Codegen** (2): ISel + encoding produce valid binary
- **Target** (4): host detection, target selection, rejection of non-host
- **JIT** (7): end-to-end compile+execute (arithmetic, branches, loops, memory, calls)
- **E2E** (4): parse .ll files from `tests/ll/` and execute
- **WASM LEB128** (3): LEB128 unsigned/signed integer decoding
- **WASM Decoder** (3): binary format parsing, section decoding, error handling
- **WASM IR** (2): WASM-to-IR conversion correctness
- **WASM JIT** (5): end-to-end WASM parse+compile+execute (constants, arithmetic, branches, loops, calls)
- **Copy-and-Patch** (7, x86_64 only): C&P backend ALU ops, shifts, div/rem, immediates, ISel fallback
- **LLVM Compat** (27, separate exe, requires `-DWITH_LLVM_COMPAT=ON`): C++ header-only LLVM API wrapper tests

Test pattern:
```c
int test_name(void) {
    // setup
    TEST_ASSERT(condition, "message");
    // cleanup
    return 0;  // 0 = pass, nonzero = fail
}
```

Register in `test_main.c`:
```c
int test_name(void);           // declaration at top
RUN_TEST(test_name);           // in main()
```

## Known Technical Debt

- WASM frontend: MVP integer subset only (no FP, SIMD, tables, bulk memory, multi-memory)
- Stack-based register allocation only (no liveness analysis)
- Copy-and-patch covers ALU ops only (single-block, i32/i64); falls back to ISel for everything else
- Stencil system is nascent (3 stencils only)
- Lexer is 46% of JIT time -- primary optimization target (#144)

## Coding Style

- C11, no external dependencies
- 4-space indent
- `lr_` prefix for all public symbols, `LR_` for constants/enums
- Types end in `_t`
- Arena allocation for all IR/MIR objects (trivial cleanup: destroy arena)
- Linked lists with intrusive `next` pointers (no dynamic arrays for IR nodes)
- Static const singletons for target registration (no dynamic loading)
