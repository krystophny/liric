# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Is Liric

Liric (Lightweight IR Compiler) is a fast, minimal JIT compiler with two frontends: LLVM IR text (.ll)
and WebAssembly binary (.wasm). Written in C11, zero external dependencies. Designed for low-latency
JIT where full LLVM overhead is unacceptable.

## Build and Test

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Clean build takes ~110ms. No external dependencies beyond a C compiler and CMake.

For LFortran mass runs, prefer:

```bash
python3 -m tools.lfortran_mass.run_mass --workers $(nproc)
```

For a full refresh (ignore cache) with optional diagnostics:

```bash
python3 -m tools.lfortran_mass.run_mass --workers $(nproc) --force
python3 -m tools.lfortran_mass.run_mass --workers $(nproc) --force \
  --diag-fail-logs --diag-jit-coredump
```

Diagnostics flags are opt-in and write into `cache/<case_id>/diag/`:
- `--diag-fail-logs`: saves stage stdout/stderr/meta for failing stages.
- `--diag-jit-coredump`: on JIT signal failures (`jit_rc < 0`), captures
  `coredumpctl info` and `eu-stack` output when available.

Useful outputs from each run:
- `/tmp/liric_lfortran_mass/summary.md`
- `/tmp/liric_lfortran_mass/results.jsonl`
- `/tmp/liric_lfortran_mass/failures.csv`

Quick SIGSEGV count (`jit_rc = -11`) from canonical results:

```bash
python3 - <<'PY'
import json
seg = 0
for line in open('/tmp/liric_lfortran_mass/results.jsonl'):
    r = json.loads(line)
    if r.get('classification') == 'liric_jit_fail' and r.get('jit_rc') == -11:
        seg += 1
print(seg)
PY
```

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
| **API wrapper** | `src/liric.c` | Thin bridge between public API and internal modules |
| **CLI** | `tools/liric_main.c` | `--jit`, `--dump-ir`, `--func` |
| **Tests** | `tests/test_*.c` | Lexer, parser, codegen, target, JIT, end-to-end |

## Architecture Details

**Core IR** (`ir.h`) is register-based SSA with explicit CFG:
- `lr_module_t` owns an arena + linked lists of functions and globals
- `lr_func_t` contains linked list of `lr_block_t`, each with linked list of `lr_inst_t`
- 34 opcodes (`LR_OP_*`): arithmetic, memory, control flow, type conversion, PHI
- Type singletons for primitives (void, i1-i64, float, double, ptr); composite types allocated per-use
- Operands are tagged unions: vreg, immediate, block ref, global ref, null, undef

**Machine IR** (`target.h`) is the ISel output:
- `lr_mfunc_t` contains linked list of `lr_mblock_t` with `lr_minst_t`
- Target-neutral `LR_MIR_*` opcodes and `LR_CC_*` condition codes shared across backends
- Each backend has its own ISel using native register numbers (x86: RAX/RCX, aarch64: X9/X10)
- Stack-based register allocation: every vreg gets a stack slot
- PHI nodes lowered as stores in predecessor blocks before terminators

**Backend interface** (`lr_target_t`):
```c
typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;
    int (*isel_func)(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod);
    int (*encode_func)(lr_mfunc_t *mf, uint8_t *buf, size_t buflen, size_t *out_len);
    int (*print_inst)(const lr_minst_t *mi, char *buf, size_t len);
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
- No floating-point codegen (types parsed but FPU/XMM instructions not emitted)
- No optimization passes on IR or MIR
- No object file emission (JIT only, no ahead-of-time compilation)
- WASM frontend: MVP integer subset only (no FP, SIMD, tables, bulk memory, multi-memory)

## Coding Style

- C11, no external dependencies
- 4-space indent
- `lr_` prefix for all public symbols, `LR_` for constants/enums
- Types end in `_t`
- Arena allocation for all IR/MIR objects (trivial cleanup: destroy arena)
- Linked lists with intrusive `next` pointers (no dynamic arrays for IR nodes)
- Static const singletons for target registration (no dynamic loading)
