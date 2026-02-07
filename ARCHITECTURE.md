# Liric Architecture

```
                        LIRIC -- Lightweight IR Compiler
                        ================================

  ┌────────────── FRONTENDS ──────────────┐  ┌────── PROGRAMMATIC API ──────┐
  │                                       │  │                              │
  │  .ll text           .wasm binary      │  │  C Builder API (builder.c)   │
  │  ┌──────────┐      ┌──────────────┐   │  │  lc_create_add/sub/...       │
  │  │ll_lexer.c│      │wasm_decode.c │   │  │       |                     │
  │  │  741 LOC │      │   361 LOC    │   │  │  C++ LLVM Compat Headers    │
  │  └────┬─────┘      └──────┬───────┘   │  │  84 files, ~5500 LOC        │
  │       |                   |            │  │  include/llvm/**/*.h         │
  │  ┌──────────┐      ┌──────────────┐   │  └──────────────────────────────┘
  │  │ll_parser │      │ wasm_to_ir.c │   │
  │  │ 1653 LOC │      │   862 LOC    │   │
  │  └────┬─────┘      └──────┬───────┘   │
  └───────┼───────────────────┼────────────┘
          └─────────┬─────────┘
                    |
  ┌─────────────────v─────────────────────────────────────────┐
  │                    lr_module_t  (SSA IR)                    │
  │                                                            │
  │  arena.c (63 LOC) -- bump allocator, all objects owned     │
  │  ir.c/h  (681 LOC) -- 34 opcodes, register-based SSA      │
  │                                                            │
  │  lr_module_t                                               │
  │    +-- lr_func_t --> lr_block_t --> lr_inst_t (linked)     │
  │    +-- lr_global_t (global variables + relocations)        │
  │    +-- lr_type_t   (singletons: primitives, per-use: comp) │
  │    +-- symbol_names[] (FNV-1a hash interning)              │
  └─────────────────┬──────────────────────────────────────────┘
                    |
  ┌─────────────────v─────────────────────────────────────────┐
  │                ISel  (Instruction Selection)               │
  │                                                            │
  │  target.h (132 LOC) -- vtable: isel_func + encode_func    │
  │                                                            │
  │  ┌──────────────────┐    ┌──────────────────┐             │
  │  │ target_x86_64.c  │    │ target_aarch64.c │             │
  │  │    1575 LOC       │    │    1414 LOC       │             │
  │  │  System V ABI     │    │  AAPCS64 ABI      │             │
  │  │  RAX/RCX scratch  │    │  X9/X10 scratch    │             │
  │  │  XMM0/1 for FP    │    │  D0/D1 for FP      │             │
  │  └──────────────────┘    └──────────────────┘             │
  │                                                            │
  │  Stack-based regalloc: every vreg -> [RBP - offset]        │
  │  MIR: 32 opcodes, target-neutral condition codes           │
  │  lr_mfunc_t -> lr_mblock_t -> lr_minst_t                  │
  └─────────────────┬──────────────────────────────────────────┘
                    |
  ┌─────────────────v─────────────────────────────────────────┐
  │                Binary Encoding + JIT                       │
  │                                                            │
  │  jit.c/h (685 LOC) -- mmap, W^X, symbol table, execution  │
  │                                                            │
  │  ┌─────────────────────────────────────────────────┐      │
  │  │  Code Buffer: 1MB mmap'd                        │      │
  │  │  Data Buffer: 256KB mmap'd                      │      │
  │  │  Symbol Table: FNV-1a hash (8192 buckets)       │      │
  │  │  Negative Cache: 4096 buckets (fast miss)       │      │
  │  │  W^X: mprotect (Linux) / MAP_JIT (macOS arm64) │      │
  │  │  icache: __builtin___clear_cache (arm64)        │      │
  │  └─────────────────────────────────────────────────┘      │
  │                                                            │
  │  Symbol resolution: JIT symbols -> dlsym(RTLD_DEFAULT)     │
  └────────────────────────────────────────────────────────────┘

  ┌─────────────── COMPAT LAYERS ─────────────────────────────┐
  │                                                            │
  │  liric_compat.c (1494 LOC) -- ~150 lc_* functions          │
  │    lc_value_t handles (VREG, CONST_INT, GLOBAL, etc.)      │
  │    lc_phi_node_t deferred PHI handling                     │
  │    lc_module_compat_t = {lr_module_t + value pool + ctx}   │
  │                                                            │
  │  include/llvm/ (84 headers, ~5500 LOC) -- LLVM 21 API     │
  │    IRBuilder<> -> lc_create_* calls                        │
  │    Module, Function, BasicBlock, Value -> wrap lc_value_t  │
  │    ExecutionSession, IRCompileLayer -> wrap lr_jit_t        │
  └────────────────────────────────────────────────────────────┘
```

## Three-Layer API

```
  Layer 3: C++ LLVM 21 Headers (include/llvm/**)
           Header-only, zero-overhead wrappers
           Drop-in replacement for #include <llvm/...>
                          |
  Layer 2: C Compat API (liric_compat.h / liric_compat.c)
           ~150 lc_* functions
           lc_value_t handle-based builder pattern
           PHI deferred finalization
                          |
  Layer 1: C Core (liric.h, ir.h, jit.h)
           lr_module_t, lr_func_t, lr_inst_t
           Arena-allocated SSA IR
           Direct JIT compilation
```

## Code Distribution

| Component | Files | LOC | % |
|-----------|------:|----:|--:|
| LLVM IR Parser | 4 | 2,580 | 12.7 |
| WASM Frontend | 4 | 1,313 | 6.5 |
| Core IR + Arena | 4 | 781 | 3.8 |
| x86_64 Backend | 2 | 1,575 | 7.8 |
| aarch64 Backend | 2 | 1,414 | 7.0 |
| JIT Engine | 2 | 685 | 3.4 |
| Builder / Public API | 3 | 573 | 2.8 |
| C Compat Layer | 2 | 1,894 | 9.3 |
| C++ LLVM Compat | 84 | 5,546 | 27.3 |
| Tests | 10 | 4,324 | 21.3 |

## Key Data Structures

**Core IR** (`ir.h`):
- `lr_module_t`: arena + linked lists of functions and globals
- `lr_func_t`: blocks linked list, param types, vreg counter
- `lr_block_t`: instruction linked list, block ID
- `lr_inst_t`: opcode + dest vreg + typed operand array
- `lr_operand_t`: tagged union (vreg, imm_i64, imm_f64, block, global, null, undef)
- `lr_type_t`: kind enum + union (array, struct, func composites)

**Machine IR** (`target.h`):
- `lr_mfunc_t`: machine blocks + stack frame layout
- `lr_mblock_t`: machine instruction list + code offset
- `lr_minst_t`: MIR opcode + dst/src machine operands + size + condition code
- `lr_moperand_t`: kind (REG, IMM, MEM, LABEL) + value union

**JIT** (`jit.h`):
- `lr_jit_t`: code/data buffers + symbol hash table + negative cache + arena
- `lr_sym_entry_t`: name + hash + addr + bucket chain

**Compat** (`liric_compat.h`):
- `lc_value_t`: kind enum + type + tagged union (vreg, const_int, const_fp, global, argument, block, aggregate)
- `lc_module_compat_t`: lr_module_t + value pool + function cache
- `lc_phi_node_t`: deferred incoming values + block IDs, finalized to lr_inst_t

## Backend Interface

```c
typedef struct lr_target {
    const char *name;       // "x86_64", "aarch64"
    uint8_t ptr_size;       // 8 for both
    int (*isel_func)(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod);
    int (*encode_func)(lr_mfunc_t *mf, uint8_t *buf, size_t buflen, size_t *out_len);
    int (*print_inst)(const lr_minst_t *mi, char *buf, size_t len);
} lr_target_t;
```

## Platform Support

- **Linux x86_64**: full support (mprotect W^X)
- **macOS arm64**: full support (MAP_JIT + pthread_jit_write_protect_np)
- JIT is host-only: `lr_jit_create()` auto-detects, cross-target fails fast
