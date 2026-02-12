# Liric -- Lightweight IR Compiler

A fast JIT compiler and native code emitter for LLVM IR and WebAssembly.
C11, zero external dependencies, ~110ms clean build.

```
             LLVM IR (.ll)    LLVM bitcode (.bc)    WebAssembly (.wasm)
                  |                   |                      |
                  v                   v                      v
             ll_lexer +          bc_decode             wasm_decode
             ll_parser                                 wasm_to_ir
                  |                   |                      |
                  +-------------------+----------------------+
                                      |
                                      v
                              lr_module_t (SSA IR)
                        arena-allocated, register-based,
                        44 opcodes, explicit CFG
                                      |
                  +-------------------+-------------------+
                  |                   |                   |
                  v                   v                   v
              ISel backend     Copy-and-patch       Real LLVM (opt)
            (single-pass,     (template memcpy,    (LLVM C API,
             full coverage)    partial coverage,    obj/exe only)
                              3.6x faster than TCC)
                  |                   |                   |
                  +-------------------+-------------------+
                  |                   |                   |
                  v                   v                   v
             JIT (mmap)        Object file (.o)      Executable
            W^X, dlsym        ELF64 / Mach-O      ELF / Mach-O
```

## Features

**Three input frontends**, auto-detected by magic bytes:
- LLVM IR text (`.ll`) -- hand-written lexer + recursive descent parser
- LLVM bitcode (`.bc`) -- native decoder, no LLVM dependency
- WebAssembly binary (`.wasm`) -- binary decoder + stack-to-SSA conversion

**Three compilation backends**, selected via `LIRIC_COMPILE_MODE` env var:
- `isel` (default) -- single-pass instruction selection + binary encoding, full opcode coverage
- `copy_patch` -- pre-assembled x86_64 templates, memcpy + patch sentinels, ISel fallback for unsupported opcodes
- `llvm` -- translates to real LLVM C API for optimized object/executable output (requires `-DWITH_REAL_LLVM_BACKEND=ON`)

**Three output modes:**
- JIT -- mmap'd code buffer with W^X transitions, symbol resolution via dlsym
- Object file -- relocatable ELF64 (Linux) or Mach-O (macOS)
- Executable -- static/dynamic ELF or Mach-O with embedded intrinsic stubs

**Four target architectures:**
- x86_64 (ISel + copy-and-patch)
- aarch64 / arm64 (ISel)
- riscv64gc (ISel)
- riscv64im (ISel)

JIT is host-only. Object/executable emission can cross-compile.

**Two programmatic APIs** for building IR without parsing text:
- Session API (`liric_session.h`) -- instruction-level construction via `lr_session_emit()`, with DIRECT mode (auto-JIT each function) and IR mode (build then emit)
- Compat API (`liric_compat.h`) -- LLVM-style `lc_value_t` handle-based builder with deferred PHI nodes, designed as drop-in replacement for LLVM C API consumers

**LLVM C++ drop-in compatibility:**
86 header-only C++17 files in `include/llvm/` map the LLVM 21 API surface (`llvm::Module`, `llvm::IRBuilder<>`, `llvm::orc::LLJIT`, etc.) to liric's C compat layer. Compilers built against LLVM can switch to liric with zero source changes by redirecting include paths.

**60 embedded intrinsic stubs** (x86_64 + aarch64 Linux):
Hand-written assembly implementations of LLVM intrinsics (`llvm.sqrt`, `llvm.exp`, `llvm.memcpy`, etc.) are embedded directly into JIT code buffers and executables, eliminating runtime library dependencies for common math and memory operations.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Optional features:

```bash
# LLVM C++ compatibility layer tests
cmake -S . -B build -G Ninja -DWITH_LLVM_COMPAT=ON

# Real LLVM backend for optimized output
cmake -S . -B build -G Ninja -DWITH_REAL_LLVM_BACKEND=ON
```

## Usage

```bash
# Emit executable (default)
./build/liric input.ll                    # -> a.out
./build/liric -o myprog input.ll          # -> myprog
./build/liric input.wasm -o myprog        # WASM input, auto-detected

# JIT compile and execute
./build/liric --jit input.ll
./build/liric --jit input.ll --func my_func
./build/liric --jit input.ll --load-lib ./libruntime.so

# Emit object file
./build/liric --emit-obj output.o input.ll
./build/liric --emit-obj output.o --target aarch64 input.ll

# Dump parsed IR (round-trip)
./build/liric --dump-ir input.ll

# Merge runtime before emission
./build/liric -o myprog input.ll --runtime runtime.ll
```

## Architecture

### Core IR

The central data structure is `lr_module_t` -- a register-based SSA IR with explicit control flow graph, 44 opcodes, and arena-allocated objects:

```
lr_module_t
  +-- lr_func_t*      functions (linked list)
  |     +-- params, return type, calling convention
  |     +-- lr_block_t*   basic blocks (linked list)
  |           +-- lr_inst_t*   instructions (linked list)
  |                 +-- opcode, dest vreg, type, operands[]
  +-- lr_global_t*   globals (linked list, with relocations)
  +-- lr_type_t      type singletons (void, i1..i64, float, double, ptr)
  +-- symbol_names   FNV-1a hash interning (4096 buckets)
```

Operands are tagged unions: vreg reference, immediate (i64/f64), block reference, global reference, null, or undef. All objects are bump-allocated from an arena -- cleanup is a single `lr_arena_destroy()`.

### Backend

Each target implements the `lr_target_t` vtable:

```c
typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;
    int (*compile_func)(...);     // ISel (always present)
    int (*compile_func_cp)(...);  // Copy-and-patch (NULL if unavailable)
} lr_target_t;
```

ISel compilation is a single fused pass per function:
1. Pre-scan: assign a stack slot to every vreg (stack-based register allocation)
2. Build PHI copies per predecessor block
3. Emit prologue (frame setup, callee-saved registers)
4. Code-gen walk: load operands to scratch registers, emit operation, store result back to stack slot
5. Emit epilogue, fixup forward branch offsets

Copy-and-patch skips ISel for supported opcodes -- it memcpy's pre-assembled templates and patches sentinel values with stack offsets. Falls back to ISel for unsupported opcodes.

### JIT Engine

The JIT engine (`jit.c`) manages:
- **Code buffer**: 16MB mmap'd region with W^X transitions (mprotect on Linux, MAP_JIT + pthread_jit_write_protect_np on macOS)
- **Data buffer**: 256KB for global variables
- **Symbol table**: 8192-bucket FNV-1a hash with three-tier resolution: JIT symbols -> loaded libraries -> dlsym(RTLD_DEFAULT)
- **Intrinsic registration**: platform-native assembly blobs for LLVM intrinsics

### Object/Executable Emission

`objfile.c` orchestrates compilation to relocatable objects or executables:
- Compiles all functions via the backend into a temp buffer
- Records symbols and relocations
- Remaps LLVM intrinsic names to platform symbols (e.g., `llvm.memcpy.p0.p0.i64` -> `memcpy`)
- Embeds intrinsic stubs for self-contained executables
- Delegates to format-specific writers: `objfile_elf.c` (ELF64) or `objfile_macho.c` (Mach-O)

### API Layers

```
Layer 3:  C++ LLVM 21 headers (include/llvm/**)
          86 header-only files, drop-in #include <llvm/...> replacement
                    |
Layer 2:  C compat API (liric_compat.h)
          ~150 lc_* functions, lc_value_t handle model, deferred PHI
                    |
Layer 1:  C session API (liric_session.h)
          lr_session_emit(), DIRECT/IR modes, unified build+compile
                    |
Layer 0:  C core (ir.h, jit.h, objfile.h)
          lr_module_t, lr_jit_t, arena allocator
```

## Platform Support

| Platform | JIT | Object File | Executable | Intrinsic Blobs |
|----------|-----|-------------|------------|-----------------|
| Linux x86_64 | yes | ELF64 | static + dynamic ELF | 60 (full) |
| Linux aarch64 | yes | ELF64 | static ELF | 26 (partial) |
| Linux riscv64 | yes | -- | static ELF | -- |
| macOS aarch64 | yes | Mach-O | Mach-O + ad-hoc codesign | -- |

## Benchmarks

```bash
# Correctness sweep (creates shared corpus)
./build/bench_compat_check --timeout 15

# Liric JIT vs lli (wall-clock + in-process comparison)
./build/bench_ll --iters 3

# Liric JIT vs lfortran LLVM native (full pipeline comparison)
./build/bench_api --iters 3

# Liric vs TCC (5 micro-benchmarks, in-process compile speed)
./build/bench_tcc --iters 10

# Focused corpus benchmark (100 curated tests, fast iteration)
./build/bench_corpus --top 10 --iters 3
```

All benchmark outputs go to `/tmp/liric_bench/`.

## Source Map

| Component | Key files | Description |
|-----------|-----------|-------------|
| Core IR | `ir.c`, `ir.h`, `arena.c` | SSA types, instructions, modules, arena allocator |
| LL frontend | `ll_lexer.c`, `ll_parser.c` | LLVM IR text tokenizer + parser |
| BC frontend | `bc_decode.c` | LLVM bitcode binary decoder |
| WASM frontend | `wasm_decode.c`, `wasm_to_ir.c` | WASM binary decoder + stack-to-SSA |
| Frontend registry | `frontend_registry.c` | Auto-detection dispatch by magic bytes |
| x86_64 backend | `target_x86_64.c`, `target_x86_64_cp.c` | ISel + copy-and-patch |
| aarch64 backend | `target_aarch64.c` | ISel |
| riscv64 backend | `target_riscv64.c` | ISel |
| Target registry | `target_registry.c` | Host detection, target lookup |
| JIT engine | `jit.c`, `jit.h` | mmap, W^X, symbol table, module compilation |
| Object emission | `objfile.c`, `objfile_elf.c`, `objfile_macho.c` | Relocatable objects + executables |
| Intrinsic stubs | `platform/` | Hand-written x86_64 + aarch64 assembly blobs |
| C&P templates | `platform/cp_templates_x86_64.S` | Pre-assembled ALU op templates |
| Session API | `session.c`, `liric_session.h` | Programmatic IR construction |
| Compat API | `liric_compat.c`, `liric_compat.h` | LLVM-style C builder |
| C++ compat | `include/llvm/**` | 86 header-only LLVM 21 API wrappers |
| LLVM backend | `llvm_backend.c` | Real LLVM C API for optimized output |
| CLI | `tools/liric_main.c` | Command-line interface |
| Tests | `tests/test_*.c`, `tests/test_llvm_compat.cpp` | Unit + integration + e2e tests |

## License

MIT
