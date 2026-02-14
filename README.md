# Liric

JIT compiler and native code emitter for LLVM IR and WebAssembly. C11, zero dependencies.

There are two compilation paths. Both produce JIT, object files, or executables.

**IR path** -- parse .ll/.bc/.wasm into SSA IR, then batch-compile:

```
  .ll / .bc / .wasm  -->  lr_module_t (SSA IR)  -->  ISel / C&P / LLVM  -->  JIT, .o, exe
```

**DIRECT path** -- stream instructions from the programmatic API, no IR:

```
  Session API  -->  ISel / C&P (streaming)  -->  JIT + blob capture  -->  JIT, .o, exe
```

The IR path is used by the CLI and `lr_session_compile_*()`. The DIRECT path is
used by the C++/C compat API (lfortran integration) for lowest-latency compilation.

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Optional: `-DWITH_LLVM_COMPAT=ON` (C++ compat tests), `-DWITH_REAL_LLVM_BACKEND=ON` (LLVM C API backend).

### LLVM Backend

When building with `-DWITH_REAL_LLVM_BACKEND=ON`, one LLVM major version is selected at configure time via `llvm-config`. Object/exe emission works with LLVM 3.8+. JIT requires LLVM 10+ (ORC v2 LLJIT APIs). C++ compat headers (`-DWITH_LLVM_COMPAT=ON`) require LLVM 11+. CI tests LLVM 3.8, 3.9, 4.0 (apt on ubuntu:16.04 containers) and every major from 5 through 21 (conda-forge).

`tools/llvm_backend_matrix.sh` runs a conda-forge compatibility sweep across all available LLVM versions (5-22).

## Usage

```bash
liric input.ll                       # emit executable (a.out)
liric -o prog input.ll               # emit executable
liric -o out.o no_main.ll            # emit relocatable object (no @main)
liric --jit input.ll                 # JIT and run
liric --jit input.ll --func f        # JIT, call f()
liric --dump-ir input.ll             # round-trip IR dump
liric -o prog input.ll --runtime rt.ll --load-lib ./libfoo.so
```

Input format is auto-detected (WASM magic, BC magic, LL text fallback).

## Frontends

| Frontend | Files | Input |
|----------|-------|-------|
| LLVM IR text | `ll_lexer.c`, `ll_parser.c` | `.ll` |
| LLVM bitcode | `bc_decode.c` | `.bc` |
| WebAssembly | `wasm_decode.c`, `wasm_to_ir.c` | `.wasm` |
| Auto-detect | `frontend_registry.c` | any of the above |

All frontends produce `lr_module_t`, a register-based SSA IR with explicit CFG.

## Backends

Selected via `LIRIC_COMPILE_MODE` env var (`isel` | `copy_patch` | `llvm`).

`LR_MODE_DIRECT` streaming fast-path is available only with `isel` or `copy_patch`.
When `LIRIC_COMPILE_MODE=llvm`, session DIRECT mode falls back to the IR path.

| Backend | Coverage | Mechanism | Targets |
|---------|----------|-----------|---------|
| ISel (default) | full | single-pass select + encode | x86_64, aarch64, riscv64 |
| Copy-and-patch | partial (ALU ops) | template memcpy + sentinel patch, ISel fallback | x86_64 |
| Real LLVM | full | LLVM C API, obj/exe only | all LLVM targets |

ISel uses stack-based register allocation (every vreg gets a stack slot, computation through scratch registers) with fused instruction selection and binary encoding in one pass.

## Output Modes

| Mode | Flag | Formats | Source |
|------|------|---------|--------|
| Executable | default / `-o` (when `@main` exists) | ELF (x86_64, aarch64, riscv64), Mach-O (aarch64) | IR or DIRECT blobs |
| JIT | `--jit` | mmap'd code, W^X, dlsym symbol resolution | IR or DIRECT streaming |
| Object file | `-o` (when `@main` is absent) | ELF64, Mach-O | IR or DIRECT blobs |
| IR dump | `--dump-ir` | LLVM IR text | IR mode only |

DIRECT mode captures relocatable machine code blobs during streaming compilation. These blobs are used directly for exe/obj emission without constructing IR.

## Programmatic APIs

For building IR without parsing text:

```
C++ LLVM 21 headers   include/llvm/**  (86 header-only wrappers, drop-in replacement)
        |
C compat API          liric_compat.h   (lc_value_t handles, deferred PHI, ~150 functions)
        |
C session API         liric_session.h  (lr_session_emit(), DIRECT/IR modes)
        |
C core                ir.h, jit.h      (lr_module_t, lr_jit_t, arena allocator)
```

The C++ headers allow LLVM-based compilers (e.g., lfortran) to switch backends with zero source changes.

**DIRECT mode** streams instructions directly to the backend (compile_begin/emit/end) without constructing persistent IR when compile mode is `isel` or `copy_patch`. The backend emits relocatable code when an `lr_objfile_ctx` is installed, capturing machine code blobs and relocation records for later exe/obj emission. JIT execution uses the same compiled code with relocations patched in-place.

## Platform Support

| Platform | JIT | Obj | Exe | Intrinsic blobs |
|----------|-----|-----|-----|-----------------|
| Linux x86_64 | yes | ELF | ELF (static/dynamic) | 60 |
| Linux aarch64 | yes | ELF | ELF (static) | 26 |
| Linux riscv64 | yes | -- | ELF (static) | -- |
| macOS aarch64 | yes | Mach-O | Mach-O + codesign | -- |

Intrinsic blobs are hand-written assembly for LLVM intrinsics (sqrt, exp, memcpy, etc.), embedded into JIT buffers and executables.

## Performance

Liric vs LLVM ORC JIT (LLJIT). Repo `tests/ll/` corpus, 8 `@main` tests, 2.42 KiB IR total.
AMD Ryzen 9 5950X, Linux 6.18, GCC 15.2, LLVM 21.1.

Liric: `liric_probe_runner --timing` (parse_us + compile_us).
LLVM ORC: `bench_lli_phases --json` (parse_ms + compile_ms, where compile = add_module + lookup since LLJIT compiles lazily on first symbol lookup).

| Phase | liric (ms) | LLVM ORC (ms) | Speedup |
|-------|------------|---------------|---------|
| Parse | 0.575 | 0.666 | 1.2x |
| Compile | 0.165 | 15.85 | **96x** |
| Total (parse+compile) | 0.740 | 16.52 | **22x** |

Liric compile averages 21 us/function. LLVM ORC averages 1.98 ms/function.

### Benchmarks

```bash
./build/bench_compat_check --timeout 15   # correctness gate
./build/bench_ll --iters 3                # liric JIT vs lli
./build/bench_api --iters 3               # lfortran LLVM vs lfortran+liric
# prerequisite for bench_corpus: populate /tmp/liric_lfortran_mass/cache
./tools/lfortran_mass/nightly_mass.sh --output-root /tmp/liric_lfortran_mass
./build/bench_corpus --iters 3            # 100-case focused corpus
./build/bench_tcc --iters 10              # liric vs TCC micro-benchmarks
```

## Source Map

All C source is in `src/`. Public headers in `include/liric/`. C++ compat headers in `include/llvm/`.

| Component | Key files |
|-----------|-----------|
| Core IR + arena | `ir.c`, `ir.h`, `arena.c` |
| LL frontend | `ll_lexer.c`, `ll_parser.c` |
| BC frontend | `bc_decode.c` |
| WASM frontend | `wasm_decode.c`, `wasm_to_ir.c` |
| x86_64 ISel + C&P | `target_x86_64.c`, `target_x86_64_cp.c` |
| aarch64 ISel | `target_aarch64.c` |
| riscv64 ISel | `target_riscv64.c` |
| JIT engine | `jit.c` |
| Object/exe emission | `objfile.c`, `objfile_elf.c`, `objfile_macho.c`, `module_emit.c` |
| Intrinsic stubs | `platform/*.S` |
| Session API | `session.c` |
| Compat API | `liric_compat.c` |
| LLVM backend | `llvm_backend.c` |
| CLI | `tools/liric_main.c` |

## License

MIT
