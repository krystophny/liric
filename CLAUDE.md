# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Is Liric

Liric (Lightweight IR Compiler) is a fast, minimal JIT compiler with two frontends: LLVM IR text (.ll)
and WebAssembly binary (.wasm). Written in C11, zero external dependencies. Designed for low-latency
JIT where full LLVM overhead is unacceptable.

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed architecture diagrams and data structures.
See [STATUS.md](STATUS.md) for current feature completion and LFortran integration progress.

## Build and Test

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

To build with the LLVM C++ compatibility layer tests:

```bash
cmake -S . -B build -G Ninja -DWITH_LLVM_COMPAT=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Clean build takes ~110ms. No external dependencies beyond a C compiler and CMake (plus a C++17 compiler when `WITH_LLVM_COMPAT=ON`).

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

All outputs go to `/tmp/liric_bench/`.

## Pipeline

```
LLVM IR text (.ll)                    WASM binary (.wasm)
    |                                      |
    v                                      v
ll_lexer --> ll_parser               wasm_decode --> wasm_to_ir
 (tokens)    (IR AST)                 (sections)    (stack->SSA)
    |                                      |
    +-------------- lr_module_t -----------+
                  (target-independent SSA IR)
                         |
                         v
                   ISel --> encode --> mmap+exec
                   (MIR)   (binary)   (JIT run)
```

The CLI auto-detects format by checking the first 4 bytes for WASM magic (`\0asm`).

## Source Map

| Component | Files | Role |
|-----------|-------|------|
| **Arena allocator** | `src/arena.c/.h` | Chunk-based bump allocator, all IR objects use it |
| **Core IR** | `src/ir.c/.h` | Types, instructions, blocks, functions, modules (target-independent SSA) |
| **LLVM IR frontend** | `src/ll_lexer.c/.h`, `src/ll_parser.c/.h` | Tokenizer + recursive descent parser for .ll text |
| **WASM frontend** | `src/wasm_decode.c/.h`, `src/wasm_to_ir.c/.h` | Binary decoder + stack-to-SSA converter for .wasm |
| **Target interface** | `src/target.h` | Backend vtable: `isel_func`, `encode_func`, `print_inst` |
| **Target registry** | `src/target_registry.c` | `lr_target_by_name()`, `lr_target_host()`, host detection |
| **x86_64 backend** | `src/target_x86_64.c/.h` | ISel (IR -> machine insts) + x86_64 binary encoder |
| **aarch64 backend** | `src/target_aarch64.c/.h` | ISel (IR -> machine insts) + aarch64 binary encoder |
| **JIT engine** | `src/jit.c/.h` | mmap, W^X transitions, symbol table, module compilation |
| **Public API** | `include/liric/liric.h` | 9 functions: parse (.ll and .wasm), module management, JIT lifecycle |
| **Compat C API** | `include/liric/liric_compat.h`, `src/liric_compat.c` | LLVM-style builder using lc_value_t handles |
| **Public types** | `include/liric/liric_types.h` | Complete type definitions for C++ compat headers |
| **LLVM C++ compat** | `include/llvm/**/*.h` | Header-only C++17 wrappers (84 headers) mapping LLVM 21 API to liric |
| **API wrapper** | `src/liric.c` | Thin bridge between public API and internal modules |
| **CLI** | `tools/liric_main.c` | `--jit`, `--dump-ir`, `--func` |
| **Tests** | `tests/test_*.c`, `tests/test_llvm_compat.cpp` | Lexer, parser, codegen, target, JIT, e2e, LLVM compat |

## Architecture Details

**Core IR** (`ir.h`) is register-based SSA with explicit CFG:
- `lr_module_t` owns an arena + linked lists of functions and globals
- `lr_func_t` contains linked list of `lr_block_t`, each with linked list of `lr_inst_t`
- 34 opcodes (`LR_OP_*`): arithmetic, memory, control flow, type conversion, PHI
- Type singletons for primitives (void, i1-i64, float, double, ptr); composite types allocated per-use
- Operands are tagged unions: vreg, immediate, block ref, global ref, null, undef

**Backend** (`target.h`) uses direct emission (no intermediate MIR):
- Single-pass `compile_func` fuses ISel + binary encoding
- Backend-local compile context holds code buffer, stack slots, and branch fixups
- `LR_CC_*` condition codes shared across backends for integer and FP comparisons
- Each backend uses native scratch registers (x86: RAX/RCX, aarch64: X9/X10)
- Stack-based register allocation: every vreg gets a stack slot
- PHI copies built before emission, applied at block terminators

**Backend interface** (`lr_target_t`):
```c
typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;
    int (*compile_func)(lr_func_t *func, lr_module_t *mod,
                        uint8_t *buf, size_t buflen, size_t *out_len,
                        lr_arena_t *arena);
} lr_target_t;
```

**JIT memory model:**
- Code buffer: 1MB mmap'd region, W^X transitions via mprotect (Linux) or MAP_JIT + pthread_jit_write_protect_np (macOS)
- Data buffer: 256KB mmap'd region (reserved, not yet used)
- Symbol resolution: JIT symbols first, then dlsym fallback
- `__builtin___clear_cache` for icache coherence on arm64

**Two compilation modes supported by the IR (not yet exposed in CLI):**
- Monolithic: all functions compiled together via `lr_jit_add_module()`
- Incremental: multiple modules can be added to one JIT instance

## Platform Support

- Linux x86_64: full support
- macOS arm64: full support (MAP_JIT, no -ldl needed)
- JIT is host-only: `lr_jit_create()` auto-detects, `lr_jit_create_for_target("other")` fails fast

## Where To Change What

- **Add a new frontend:** Parse input format into `lr_module_t` using the `ir.h` API. Register in public API.
- **Add/modify a backend:** Implement `lr_target_t` vtable in `src/target_<name>.c`, register in `src/target_registry.c`
- **Add an instruction:** Add opcode to `lr_opcode_t` in `ir.h`, handle in parser, ISel, and encoder
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
- **Builder** (9): C builder API for programmatic IR construction
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

- Parser limits are hardcoded (4096 vregs, 1024 blocks, 1024 functions, 4096 globals)
- No optimization passes on IR or MIR (issue #42)
- No object file emission (JIT only, no ahead-of-time compilation)
- WASM frontend: MVP integer subset only (no FP, SIMD, tables, bulk memory, multi-memory)
- Stack-based register allocation only (no liveness analysis)
- LFortran mass tests: 174/2415 passing (see STATUS.md for breakdown)

## Coding Style

- C11, no external dependencies
- 4-space indent
- `lr_` prefix for all public symbols, `LR_` for constants/enums
- Types end in `_t`
- Arena allocation for all IR/MIR objects (trivial cleanup: destroy arena)
- Linked lists with intrusive `next` pointers (no dynamic arrays for IR nodes)
- Static const singletons for target registration (no dynamic loading)
