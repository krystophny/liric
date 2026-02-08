# Liric Architecture

Liric (Lightweight IR Compiler) is a fast, minimal JIT compiler and object file emitter
for LLVM IR (`.ll`) and WebAssembly (`.wasm`). C11, zero dependencies. ~110ms clean build.

All source links below are GitHub permalinks to commit `200b29c`.

```
                        LIRIC -- Lightweight IR Compiler
                        ================================

  ┌────────────── FRONTENDS ──────────────┐  ┌────── PROGRAMMATIC API ──────┐
  │                                       │  │                              │
  │  .ll text           .wasm binary      │  │  C Builder API (builder.c)   │
  │  ┌──────────┐      ┌──────────────┐   │  │  lc_create_add/sub/...       │
  │  │ll_lexer.c│      │wasm_decode.c │   │  │       |                     │
  │  │  822 LOC │      │   361 LOC    │   │  │  C++ LLVM Compat Headers    │
  │  └────┬─────┘      └──────┬───────┘   │  │  84 files, ~5000 LOC        │
  │       |                   |            │  │  include/llvm/**/*.h         │
  │  ┌──────────┐      ┌──────────────┐   │  └──────────────────────────────┘
  │  │ll_parser │      │ wasm_to_ir.c │   │
  │  │ 1791 LOC │      │   862 LOC    │   │
  │  └────┬─────┘      └──────┬───────┘   │
  └───────┼───────────────────┼────────────┘
          └─────────┬─────────┘
                    |
  ┌─────────────────v─────────────────────────────────────────┐
  │                    lr_module_t  (SSA IR)                    │
  │                                                            │
  │  arena.c (63 LOC) -- bump allocator, all objects owned     │
  │  ir.c/h  (892 LOC) -- 42 opcodes, register-based SSA      │
  │                                                            │
  │  lr_module_t                                               │
  │    +-- lr_func_t --> lr_block_t --> lr_inst_t (linked)     │
  │    +-- lr_global_t (global variables + relocations)        │
  │    +-- lr_type_t   (singletons: primitives, per-use: comp) │
  │    +-- symbol_names[] (FNV-1a hash interning)              │
  └─────────────────┬──────────────────────────────────────────┘
                    |
            ┌───────┴───────┐
            |               |
  ┌─────────v────────┐  ┌──v──────────────────────────────────┐
  │   JIT Path        │  │  Object File Emission Path          │
  │                   │  │                                      │
  │  jit.c (617 LOC)  │  │  objfile.c (246 LOC) -- orchestrator│
  │  mmap, W^X,       │  │  objfile_elf.c (382 LOC) -- ELF64   │
  │  symbol table,    │  │  objfile_macho.c (288 LOC) -- Mach-O│
  │  dlsym fallback   │  │                                      │
  └─────────┬─────────┘  └──┬──────────────────────────────────┘
            |               |
  ┌─────────v───────────────v─────────────────────────────────┐
  │                ISel + Binary Encoding                       │
  │                                                            │
  │  target.h -- vtable: compile_func (single-pass)            │
  │                                                            │
  │  ┌──────────────────┐    ┌──────────────────┐             │
  │  │ target_x86_64.c  │    │ target_aarch64.c │             │
  │  │  1561 LOC         │    │  1183 LOC         │             │
  │  │  System V ABI     │    │  AAPCS64 ABI      │             │
  │  │  RAX/RCX scratch  │    │  X9/X10 scratch    │             │
  │  │  XMM0/1 for FP    │    │  D0/D1 for FP      │             │
  │  └──────────────────┘    └──────────────────┘             │
  │                                                            │
  │  Stack-based regalloc: every vreg -> [FP - offset]         │
  │  Direct emission: ISel + encoding fused in one pass        │
  │  Pre-scan allocates all slots, then single code-gen walk   │
  └────────────────────────────────────────────────────────────┘

  ┌─────────────── COMPAT LAYERS ─────────────────────────────┐
  │                                                            │
  │  liric_compat.c (1681 LOC) -- ~150 lc_* functions          │
  │    lc_value_t handles (VREG, CONST_INT, GLOBAL, etc.)      │
  │    lc_phi_node_t deferred PHI handling                     │
  │    lc_module_compat_t = {lr_module_t + value pool + ctx}   │
  │                                                            │
  │  include/llvm/ (84 headers, ~5000 LOC) -- LLVM 21 API     │
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
           Direct JIT compilation + object file emission
```

## Source Map

| Component | Files | LOC | Description |
|-----------|-------|----:|-------------|
| **Arena allocator** | [arena.c](https://github.com/krystophny/liric/blob/200b29c/src/arena.c), [arena.h](https://github.com/krystophny/liric/blob/200b29c/src/arena.h) | 90 | Chunk-based bump allocator, all IR objects use it |
| **Core IR** | [ir.c](https://github.com/krystophny/liric/blob/200b29c/src/ir.c), [ir.h](https://github.com/krystophny/liric/blob/200b29c/src/ir.h) | 892 | Types, 42 opcodes, instructions, blocks, functions, modules (SSA) |
| **LLVM IR lexer** | [ll_lexer.c](https://github.com/krystophny/liric/blob/200b29c/src/ll_lexer.c), [ll_lexer.h](https://github.com/krystophny/liric/blob/200b29c/src/ll_lexer.h) | 998 | Tokenizer for .ll text format |
| **LLVM IR parser** | [ll_parser.c](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c), [ll_parser.h](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.h) | 1801 | Recursive descent parser, builds `lr_module_t` from tokens |
| **WASM decoder** | [wasm_decode.c](https://github.com/krystophny/liric/blob/200b29c/src/wasm_decode.c), [wasm_decode.h](https://github.com/krystophny/liric/blob/200b29c/src/wasm_decode.h) | 441 | Binary format parser, section decoding, LEB128 |
| **WASM-to-IR** | [wasm_to_ir.c](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.c), [wasm_to_ir.h](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.h) | 872 | Stack-to-SSA converter, builds `lr_module_t` from WASM sections |
| **Target interface** | [target.h](https://github.com/krystophny/liric/blob/200b29c/src/target.h) | 35 | Backend vtable: `compile_func` (single-pass ISel + encoding) |
| **Target registry** | [target_registry.c](https://github.com/krystophny/liric/blob/200b29c/src/target_registry.c) | 33 | `lr_target_by_name()`, `lr_target_host()`, host detection |
| **x86_64 backend** | [target_x86_64.c](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c), [target_x86_64.h](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.h) | 1593 | ISel + x86_64 binary encoder, System V ABI |
| **aarch64 backend** | [target_aarch64.c](https://github.com/krystophny/liric/blob/200b29c/src/target_aarch64.c), [target_aarch64.h](https://github.com/krystophny/liric/blob/200b29c/src/target_aarch64.h) | 1208 | ISel + aarch64 binary encoder, AAPCS64 ABI |
| **JIT engine** | [jit.c](https://github.com/krystophny/liric/blob/200b29c/src/jit.c), [jit.h](https://github.com/krystophny/liric/blob/200b29c/src/jit.h) | 688 | mmap, W^X transitions, symbol table, module compilation |
| **Object file emission** | [objfile.c](https://github.com/krystophny/liric/blob/200b29c/src/objfile.c), [objfile.h](https://github.com/krystophny/liric/blob/200b29c/src/objfile.h) | 353 | Orchestrates backend compilation + format writing |
| **ELF writer** | [objfile_elf.c](https://github.com/krystophny/liric/blob/200b29c/src/objfile_elf.c), [objfile_elf.h](https://github.com/krystophny/liric/blob/200b29c/src/objfile_elf.h) | 395 | ELF64 relocatable object emission (Linux) |
| **Mach-O writer** | [objfile_macho.c](https://github.com/krystophny/liric/blob/200b29c/src/objfile_macho.c), [objfile_macho.h](https://github.com/krystophny/liric/blob/200b29c/src/objfile_macho.h) | 301 | Mach-O object emission (macOS) |
| **Builder API** | [builder.c](https://github.com/krystophny/liric/blob/200b29c/src/builder.c) | 526 | C builder for programmatic IR construction |
| **Public API** | [liric.h](https://github.com/krystophny/liric/blob/200b29c/include/liric/liric.h), [liric.c](https://github.com/krystophny/liric/blob/200b29c/src/liric.c) | 265 | Parse (.ll/.wasm), module management, JIT lifecycle |
| **C compat layer** | [liric_compat.h](https://github.com/krystophny/liric/blob/200b29c/include/liric/liric_compat.h), [liric_compat.c](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c) | 2099 | ~150 `lc_*` functions, handle-based builder, PHI deferred finalization |
| **Public types** | [liric_types.h](https://github.com/krystophny/liric/blob/200b29c/include/liric/liric_types.h) | 150 | Complete type definitions for C++ compat headers |
| **C++ LLVM compat** | [include/llvm/\*\*/\*.h](https://github.com/krystophny/liric/tree/200b29c/include/llvm) | ~5000 | 84 header-only C++17 wrappers mapping LLVM 21 API to liric |

## Core IR

The IR ([ir.h](https://github.com/krystophny/liric/blob/200b29c/src/ir.h)) is register-based SSA with explicit CFG.

### Type System

[`lr_type_kind_t`](https://github.com/krystophny/liric/blob/200b29c/src/ir.h#L8-L21) — 12 type kinds:

| Kind | Description |
|------|-------------|
| `LR_TYPE_VOID` | void |
| `LR_TYPE_I1` .. `LR_TYPE_I64` | integer types (1, 8, 16, 32, 64 bit) |
| `LR_TYPE_FLOAT`, `LR_TYPE_DOUBLE` | IEEE 754 floating point |
| `LR_TYPE_PTR` | opaque pointer |
| `LR_TYPE_ARRAY` | `{elem, count}` — fixed-size array |
| `LR_TYPE_STRUCT` | `{fields[], num_fields, packed, name}` — named or anonymous struct |
| `LR_TYPE_FUNC` | `{ret, params[], num_params, vararg}` — function signature |

Primitive types are singletons cached on `lr_module_t` (e.g., `m->type_i32`). Composite types
are allocated per-use via the arena.

### Opcodes

[`lr_opcode_t`](https://github.com/krystophny/liric/blob/200b29c/src/ir.h#L32-L75) — 42 opcodes:

| Category | Opcodes |
|----------|---------|
| **Control flow** | `RET`, `RET_VOID`, `BR`, `CONDBR`, `UNREACHABLE` |
| **Integer arithmetic** | `ADD`, `SUB`, `MUL`, `SDIV`, `SREM` |
| **Bitwise** | `AND`, `OR`, `XOR`, `SHL`, `LSHR`, `ASHR` |
| **Float arithmetic** | `FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG` |
| **Comparison** | `ICMP`, `FCMP` |
| **Memory** | `ALLOCA`, `LOAD`, `STORE`, `GEP` |
| **Calls** | `CALL` |
| **SSA** | `PHI`, `SELECT` |
| **Type conversion** | `SEXT`, `ZEXT`, `TRUNC`, `BITCAST`, `PTRTOINT`, `INTTOPTR`, `SITOFP`, `FPTOSI`, `FPEXT`, `FPTRUNC` |
| **Aggregate** | `EXTRACTVALUE`, `INSERTVALUE` |

### IR Object Hierarchy

```
lr_module_t (owns arena)
  ├── type singletons: type_void, type_i1, type_i8, ..., type_double, type_ptr
  ├── lr_func_t* functions (linked list)
  │     ├── name, return type, param types[], param_vregs[]
  │     ├── vreg_counter (SSA numbering)
  │     └── lr_block_t* blocks (linked list)
  │           ├── block_id, name
  │           └── lr_inst_t* instructions (linked list, first/last)
  │                 ├── lr_opcode_t op
  │                 ├── uint32_t dest (vreg)
  │                 ├── lr_type_t* type
  │                 └── lr_operand_t operands[] (tagged union)
  ├── lr_global_t* globals (linked list)
  │     ├── name, type, init_data, init_size
  │     └── lr_global_reloc_t* relocs (linked list)
  └── symbol_names[] (FNV-1a hash interning, 4096 buckets)
```

### Operands

[`lr_operand_t`](https://github.com/krystophny/liric/blob/200b29c/src/ir.h#L88-L107) — tagged union:

| Tag | Payload | Usage |
|-----|---------|-------|
| `LR_OPERAND_VREG` | `vreg_id` + type | SSA value reference |
| `LR_OPERAND_IMM_I64` | `int64_t` + type | Integer constant |
| `LR_OPERAND_IMM_F64` | `double` + type | Float/double constant |
| `LR_OPERAND_BLOCK` | `block_id` | Branch target |
| `LR_OPERAND_GLOBAL` | `global_id` | Global variable reference |
| `LR_OPERAND_NULL` | type | Null pointer constant |
| `LR_OPERAND_UNDEF` | type | Undefined value |

## Arena Allocator

[`arena.c`](https://github.com/krystophny/liric/blob/200b29c/src/arena.c) — 63 lines, chunk-based bump allocator.

All IR objects (`lr_func_t`, `lr_block_t`, `lr_inst_t`, `lr_type_t`, `lr_global_t`) are
arena-allocated. Cleanup is trivial: destroy the arena and all objects are freed at once.
No individual `free()` calls needed for IR nodes.

Key functions:
- [`lr_arena_create()`](https://github.com/krystophny/liric/blob/200b29c/src/arena.c#L12) — create arena with default 64KB chunks
- `lr_arena_alloc()` — bump-allocate N bytes, grow chunk if needed
- `lr_arena_new(arena, Type)` — allocate a single typed object (macro)
- `lr_arena_array(arena, Type, N)` — allocate array of N elements (macro)
- `lr_arena_strdup()` — duplicate string into arena
- `lr_arena_destroy()` — free all chunks

## LLVM IR Frontend

Two-stage pipeline: lexer -> parser.

### Lexer

[`ll_lexer.c`](https://github.com/krystophny/liric/blob/200b29c/src/ll_lexer.c) (822 LOC) — hand-written tokenizer.

Produces tokens for LLVM IR text: keywords (`define`, `declare`, `global`, type names),
identifiers (`@global`, `%local`, `%42`), literals (integer, float, string), operators,
and structural tokens (braces, parens, commas).

### Parser

[`ll_parser.c`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c) (1791 LOC) — recursive descent, single-pass.

Entry: [`lr_parse_ll_text()`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c#L1740) which calls:
- [`parse_function_def()`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c#L1510) — `define`/`declare` including params, attributes
- [`parse_global()`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c#L1590) — `@name = global/constant type initializer`
- [`parse_instruction()`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c#L800) — all 42 opcodes
- [`parse_const_gep_operand()`](https://github.com/krystophny/liric/blob/200b29c/src/ll_parser.c#L555) — constant GEP folding (computes byte offsets at parse time using [`lr_type_size()`](https://github.com/krystophny/liric/blob/200b29c/src/ir.c#L272))

Uses three index tables (FNV-1a hash, 4096 buckets each) for O(1) lookup of:
- vreg names -> vreg IDs
- block labels -> block IDs
- global names -> global IDs

## WASM Frontend

Two-stage pipeline: binary decoder -> stack-to-SSA converter.

### Decoder

[`wasm_decode.c`](https://github.com/krystophny/liric/blob/200b29c/src/wasm_decode.c) (361 LOC) — parses WASM binary format.

Decodes sections: type, import, function, export, code, data. Uses LEB128 variable-length
integer encoding. Detects format by checking first 4 bytes for WASM magic (`\0asm`).

### Stack-to-SSA Converter

[`wasm_to_ir.c`](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.c) (862 LOC) — converts WASM stack machine to SSA IR.

Entry: [`lr_wasm_to_ir()`](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.c#L781) which:
1. Maps WASM types to `lr_type_t` via [`wasm_to_lr_type()`](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.c#L53)
2. Creates `lr_func_t` for each WASM function (imports as declarations, code as definitions)
3. Converts each function body via [`convert_func_body()`](https://github.com/krystophny/liric/blob/200b29c/src/wasm_to_ir.c#L350) which maintains an explicit value stack and translates WASM opcodes to SSA instructions with explicit vreg assignments

MVP integer subset only (no FP, SIMD, tables, bulk memory).

## Backend Architecture

### Target Interface

[`target.h`](https://github.com/krystophny/liric/blob/200b29c/src/target.h) defines the backend vtable:

```c
typedef struct lr_target {
    const char *name;       // "x86_64", "aarch64"
    uint8_t ptr_size;       // 8 for both
    int (*compile_func)(lr_func_t *func, lr_module_t *mod,
                        uint8_t *buf, size_t buflen, size_t *out_len,
                        lr_arena_t *arena);
} lr_target_t;
```

[`target_registry.c`](https://github.com/krystophny/liric/blob/200b29c/src/target_registry.c) provides:
- [`lr_target_host()`](https://github.com/krystophny/liric/blob/200b29c/src/target_registry.c#L17) — auto-detect host target at compile time
- [`lr_target_by_name()`](https://github.com/krystophny/liric/blob/200b29c/src/target_registry.c#L7) — lookup by name string

### Compilation Flow (per function)

Both backends follow the same pattern (shown for x86_64):

[`x86_64_compile_func()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L903):

1. **Pre-scan** ([`prescan_slots()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L793)) — walk all instructions, assign a stack slot to every vreg. Stack-based register allocation: every virtual register gets `[RBP - offset]`.

2. **Build PHI copies** ([`build_phi_copies()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L861)) — pre-compute PHI incoming values per predecessor block. These get emitted at block terminators before branches.

3. **Emit prologue** ([`emit_prologue()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L897)) — push RBP, set up frame, allocate stack space, save callee-saved registers.

4. **Store parameters** — move register/stack arguments to their assigned stack slots. x86_64 uses System V ABI: RDI, RSI, RDX, RCX, R8, R9 for integer args; XMM0-XMM7 for FP.

5. **Code-gen walk** — single linear pass over all blocks and instructions:
   - Load operands from stack slots to scratch registers (RAX, RCX for integer; XMM0, XMM1 for FP)
   - Emit the operation (ALU, branch, call, memory access, etc.)
   - Store result back to the destination vreg's stack slot
   - At block terminators, emit PHI copies then branch

6. **Emit epilogue** ([`emit_epilogue()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L897)) — restore callee-saved registers, restore RSP/RBP, return.

7. **Fixup branches** — patch forward branch offsets that weren't known during emission.

### x86_64 Backend

[`target_x86_64.c`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c) (1561 LOC):
- System V AMD64 ABI calling convention
- Scratch registers: RAX, RCX (integer), XMM0, XMM1 (FP)
- Direct x86_64 binary encoding (REX prefixes, ModR/M, SIB, immediate operands)
- [`encode_alu_rr()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L130), [`encode_mem()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L166), [`encode_sse_rr()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L227) — low-level byte emitters
- [`emit_load_operand()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L476) / [`emit_store_slot()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L432) — vreg stack slot access
- GEP ([`aggregate_index_path()`](https://github.com/krystophny/liric/blob/200b29c/src/target_x86_64.c#L72)), struct field access, extractvalue/insertvalue
- Call emission with ABI-correct argument passing and stack alignment

### aarch64 Backend

[`target_aarch64.c`](https://github.com/krystophny/liric/blob/200b29c/src/target_aarch64.c) (1183 LOC):
- AAPCS64 calling convention
- Scratch registers: X9, X10 (integer), D0, D1 (FP)
- ARM64 fixed-width 32-bit instruction encoding
- [`aarch64_compile_func()`](https://github.com/krystophny/liric/blob/200b29c/src/target_aarch64.c#L602) — same pattern as x86_64

## JIT Engine

[`jit.c`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c) (617 LOC) — in-memory compilation and execution.

### Memory Model

```
┌────────────────────────────────────────┐
│  Code Buffer: 1MB mmap'd              │
│  W^X transitions via mprotect (Linux)  │
│  or MAP_JIT (macOS arm64)              │
├────────────────────────────────────────┤
│  Data Buffer: 256KB mmap'd            │
│  Globals materialized here             │
├────────────────────────────────────────┤
│  Symbol Table: FNV-1a hash             │
│  8192 buckets, arena-allocated entries  │
│  Negative cache: 4096 buckets          │
└────────────────────────────────────────┘
```

### Module Compilation Flow

[`lr_jit_add_module()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L491):

1. **Make writable** ([`make_writable()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L78)) — `mprotect(PROT_READ|PROT_WRITE)` on code buffer
2. **Materialize globals** ([`materialize_module_globals()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L400)) — allocate global data, copy initializers, register symbols
3. **Pre-register functions** — check which functions already exist (for incremental compilation)
4. **Compile loop** — for each uncompiled function:
   - [`resolve_global_operands()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L460) — patch global references (function pointers, global addresses)
   - `target->compile_func()` — backend compiles to machine code in-place in the code buffer
   - [`lr_jit_add_symbol()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L302) — register compiled function address
5. **Apply global relocations** ([`apply_module_global_relocs()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L374)) — patch data-to-code and data-to-data references
6. **Make executable** ([`make_executable()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L93)) — `mprotect(PROT_READ|PROT_EXEC)` + icache flush on arm64

### Symbol Resolution

[`lookup_symbol()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L340) — three-tier resolution:

1. **JIT symbol table** — hash lookup in 8192-bucket table
2. **Loaded libraries** — `dlsym()` on explicitly loaded shared libraries
3. **Process symbols** — `dlsym(RTLD_DEFAULT)` fallback for libc, runtime, etc.

Negative cache (4096 buckets) avoids repeated `dlsym` calls for symbols that don't exist.

### Builtin Symbols

[`register_builtin_symbols()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L327) pre-registers LLVM intrinsic implementations:
- `llvm.fabs.f32/f64` -> `fabsf`/`fabs`
- `llvm.sqrt.f32` -> `sqrtf`
- `llvm.exp.f32` -> `expf`
- `llvm.copysign.f32` -> `copysignf`
- `llvm.memset.p0i8.i32` -> `memset`
- `llvm.memcpy.p0i8.p0i8.{i32,i64}` -> `memcpy`

## Object File Emission

[`objfile.c`](https://github.com/krystophny/liric/blob/200b29c/src/objfile.c) (246 LOC) — orchestrates compilation to relocatable object files.

### Flow

[`lr_emit_object()`](https://github.com/krystophny/liric/blob/200b29c/src/objfile.c#L109):

1. Allocate temporary code buffer (1MB) and data buffer (256KB)
2. For each function: compile via `target->compile_func()`, record symbol + relocations
3. For each global: allocate in data buffer, copy initializer, record symbol
4. Record undefined symbols (external references)
5. Delegate to format-specific writer:
   - Linux: [`write_elf()`](https://github.com/krystophny/liric/blob/200b29c/src/objfile_elf.c#L94) — ELF64 with `.text`, `.data`, `.bss`, `.symtab`, `.strtab`, `.rela.text`
   - macOS: [`write_macho()`](https://github.com/krystophny/liric/blob/200b29c/src/objfile_macho.c#L40) — Mach-O with `__TEXT,__text`, `__DATA,__data`

### Intrinsic Remapping

[`remap_intrinsic()`](https://github.com/krystophny/liric/blob/200b29c/src/objfile.c#L13) converts LLVM intrinsic names to platform symbols in object files (e.g., `llvm.memcpy.p0i8.p0i8.i64` -> `memcpy`), since the linker needs real symbol names.

### Relocation Types

[`objfile.h`](https://github.com/krystophny/liric/blob/200b29c/src/objfile.h#L10-L25):

| Platform | Type | Usage |
|----------|------|-------|
| ARM64 | `BRANCH26` | Direct call (B/BL) |
| ARM64 | `PAGE21` / `PAGEOFF12` | ADRP + LDR/STR for globals |
| ARM64 | `GOT_LOAD_PAGE21` / `GOT_LOAD_PAGEOFF12` | GOT-indirect loads |
| x86_64 | `PC32` / `PLT32` | RIP-relative call/branch |
| x86_64 | `GOTPCREL` | GOT-indirect loads |
| x86_64 | `64` | Absolute 64-bit address |

## Compat Layer

The compat layer enables lfortran (and other LLVM-based compilers) to use liric as a
drop-in LLVM replacement with zero source changes.

### C Compat API

[`liric_compat.c`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c) (1681 LOC) — ~150 `lc_*` functions.

Central abstraction: [`lc_value_t`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L34-L58) — a handle that wraps different IR value kinds:

| Kind | Description |
|------|-------------|
| `LC_VAL_VREG` | SSA virtual register (result of an instruction) |
| `LC_VAL_CONST_INT` | Integer constant |
| `LC_VAL_CONST_FP` | Float/double constant |
| `LC_VAL_GLOBAL` | Global variable reference |
| `LC_VAL_ARGUMENT` | Function parameter |
| `LC_VAL_BLOCK` | Basic block reference |
| `LC_VAL_AGGREGATE` | Aggregate constant (struct/array) |
| `LC_VAL_NULL` | Null pointer |
| `LC_VAL_UNDEF` | Undefined value |
| `LC_VAL_FUNC` | Function reference |

[`lc_value_to_desc()`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L108) converts `lc_value_t` to `lr_operand_desc_t` (the core IR's operand format). This is the bridge between the handle-based compat API and the low-level IR.

**PHI handling**: [`lc_phi_node_t`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L68-L80) stores incoming values + blocks in deferred lists, finalized to `lr_inst_t` by [`lc_phi_finalize()`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L1344) when all predecessors are known.

**Object emission**: [`lc_module_emit_object_to_file()`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L1663) calls [`lr_emit_object()`](https://github.com/krystophny/liric/blob/200b29c/src/objfile.c#L109) via [`lr_target_host()`](https://github.com/krystophny/liric/blob/200b29c/src/target_registry.c#L17).

### C++ LLVM Compat Headers

[`include/llvm/`](https://github.com/krystophny/liric/tree/200b29c/include/llvm) — 84 header-only C++17 files (~5000 LOC).

These provide the exact same `#include <llvm/...>` paths and class/method names as LLVM 21.
lfortran compiled with `-DWITH_LIRIC=ON` uses these headers instead of real LLVM headers,
requiring zero source changes.

Key mappings:
- `llvm::Module` -> wraps `lc_module_compat_t`
- `llvm::Function` / `llvm::BasicBlock` / `llvm::Value` -> wrap `lc_value_t`
- `llvm::IRBuilder<>` -> methods call `lc_create_add()`, `lc_create_store()`, etc.
- `llvm::orc::LLJIT` -> wraps `lr_jit_t`
- `llvm::TargetMachine::emitToFile()` -> calls [`lc_module_emit_object_to_file()`](https://github.com/krystophny/liric/blob/200b29c/src/liric_compat.c#L1663)

## Platform Support

| Platform | JIT | Object Files | Notes |
|----------|-----|-------------|-------|
| Linux x86_64 | full | ELF64 | `mprotect` W^X, `-ldl` for `dlsym` |
| macOS arm64 | full | Mach-O | `MAP_JIT` + `pthread_jit_write_protect_np`, icache flush |

JIT is host-only: [`lr_jit_create()`](https://github.com/krystophny/liric/blob/200b29c/src/jit.c#L263) auto-detects, cross-target fails fast.
Object file emission can target either format from either host.

## Code Distribution

| Component | Files | LOC | % |
|-----------|------:|----:|--:|
| LLVM IR Frontend | 4 | 2,799 | 15.3 |
| WASM Frontend | 4 | 1,313 | 7.2 |
| Core IR + Arena | 4 | 982 | 5.4 |
| x86_64 Backend | 2 | 1,593 | 8.7 |
| aarch64 Backend | 2 | 1,208 | 6.6 |
| JIT Engine | 2 | 688 | 3.8 |
| Object File Emission | 6 | 1,049 | 5.7 |
| Builder / Public API | 3 | 791 | 4.3 |
| C Compat Layer | 2 | 2,099 | 11.5 |
| C++ LLVM Compat | 84 | ~5,000 | 27.3 |
| Tests | 10 | ~4,300 | - |
