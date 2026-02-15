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
Optional install compatibility: `-DLIRIC_INSTALL_COMPAT_HEADERS=ON` installs deprecated `<liric/liric_compat.h>` forwarding header.

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
When compile mode is `llvm`, DIRECT mode hard-fails (no fallback).

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

For building and compiling without invoking the CLI:

```
C public API (default) include/liric/liric.h       (lr_compiler_* unified API)
        |
Session API           include/liric/liric_session.h (DIRECT/IR, internal/advanced)
        |
C++ LLVM 21 headers   include/llvm/**              (header-only wrappers)
        |
LLVM-compat C shim    include/llvm-c/**            (LLVMLiric* + lc_* surface)
        |
C core                ir.h, jit.h                  (lr_module_t, lr_jit_t, arena allocator)
```

The C++ headers allow LLVM-based compilers (e.g., lfortran) to switch backends with zero source changes.

**DIRECT mode** streams instructions directly to the backend (compile_begin/emit/end) without constructing persistent IR when compile mode is `isel` or `copy_patch`. The backend emits relocatable code when an `lr_objfile_ctx` is installed, capturing machine code blobs and relocation records for later exe/obj emission. JIT execution uses the same compiled code with relocations patched in-place.

`lr_compiler_create(NULL, ...)` defaults to DIRECT + ISEL and does not read `LIRIC_COMPILE_MODE`. Backend/policy selection is explicit via `lr_compiler_config_t`.

### LLVM C Symbol Namespace

Liric intentionally does not export the exact LLVM C API symbol names. The shim uses `LLVMLiric*` (and `lc_*`) names to avoid linker symbol collisions with real LLVM.

If an integration chooses to use real LLVM C API names, it must choose one implementation at link time (typically via build flags/ifdefs) and avoid linking both implementations into the same process.

## Platform Support

| Platform | JIT | Obj | Exe | Intrinsic blobs |
|----------|-----|-----|-----|-----------------|
| Linux x86_64 | yes | ELF | ELF (static/dynamic) | 60 |
| Linux aarch64 | yes | ELF | ELF (static) | 26 |
| Linux riscv64 | yes | -- | ELF (static) | -- |
| macOS aarch64 | yes | Mach-O | Mach-O + codesign | -- |

Intrinsic blobs are hand-written assembly for LLVM intrinsics (sqrt, exp, memcpy, etc.), embedded into JIT buffers and executables.

## Performance

Published README numbers are generated artifacts (date + commit + host + toolchain), not free-form text.

Last published snapshot files:
- `docs/benchmarks/readme_perf_snapshot.json`
- `docs/benchmarks/readme_perf_table.md`

Regenerate end-to-end:

```bash
./tools/bench_readme_perf_snapshot.sh \
  --build-dir ./build \
  --bench-dir /tmp/liric_bench \
  --out-dir docs/benchmarks \
  --iters 3 \
  --timeout 30 \
  --corpus tools/corpus_100.tsv \
  --cache-dir /tmp/liric_lfortran_mass/cache
```

Validate published README benchmark artifacts:

```bash
./tools/bench_readme_perf_gate.sh
```

### Benchmarks

```bash
# canonical: all modes + all lanes + strict matrix accounting
./build/bench_matrix --manifest tools/bench_manifest.json --modes all --lanes all --iters 1

# lane tools (bench_matrix calls these internally)
./build/bench_compat_check --timeout 15   # correctness gate
./build/bench_ll --iters 3                # liric JIT vs lli
./build/bench_api --iters 3               # lfortran LLVM vs lfortran+liric
# prerequisite for bench_corpus: populate /tmp/liric_lfortran_mass/cache
./tools/lfortran_mass/nightly_mass.sh --output-root /tmp/liric_lfortran_mass
./build/bench_corpus --iters 3            # 100-case focused corpus
./build/bench_corpus_compare --iters 3    # real corpus compare: single canonical corpus lane
./build/bench_tcc --iters 10              # liric vs TCC micro-benchmarks
./build/bench_exe_matrix --iters 3        # ll->exe matrix: isel/copy_patch/llvm vs clang baseline
```

## Source Map

All C source is in `src/`. Public headers in `include/liric/`. C++ compat headers in `include/llvm/`. LLVM-compatible C shim headers are in `include/llvm-c/`.

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
