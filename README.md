# Liric

JIT compiler and native code emitter for LLVM IR and WebAssembly. C11, zero dependencies.

```
         .ll text    .bc bitcode    .wasm binary       Session / Compat API
             |            |              |                      |
             v            v              v                      |
          ll_lexer    bc_decode     wasm_decode                 |
          ll_parser                 wasm_to_ir                  |
             |            |              |                      |
             +------------+--------------+----------------------+
                                         |
                                    lr_module_t
                                    (SSA IR, 45 ops)
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

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Optional: `-DWITH_LLVM_COMPAT=ON` (C++ compat tests), `-DWITH_REAL_LLVM_BACKEND=ON` (LLVM C API backend).

### LLVM Backend/Compat Policy

- One LLVM major version is selected at configure time via `llvm-config`.
- Backend selection is link-time only; no runtime `dlopen` dispatch is required.
- Unsupported mixed mode now fails fast at configure time.

| LLVM major | `WITH_REAL_LLVM_BACKEND=ON` | `WITH_REAL_LLVM_BACKEND=ON` + `WITH_LLVM_COMPAT=ON` |
|------------|-----------------------------|------------------------------------------------------|
| 7-10 | Supported | Unsupported (configure-time error) |
| 11+ | Supported | Supported |

Conda compatibility sweep:

- `tools/llvm_backend_matrix.sh` requests LLVM majors `1..22` by default.
- The script discovers published `llvmdev` majors from conda-forge and skips missing versions (for example, majors that are not published yet).
- Majors `<7` are reported as unsupported by liric policy and skipped unless `--strict-versions` is passed.
- `--list-available` prints discoverable conda `llvmdev` majors without building.

Examples:

```bash
tools/llvm_backend_matrix.sh
tools/llvm_backend_matrix.sh --list-available
tools/llvm_backend_matrix.sh --versions "7 11 21" --strict-versions
```

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

| Backend | Coverage | Mechanism | Targets |
|---------|----------|-----------|---------|
| ISel (default) | full | single-pass select + encode | x86_64, aarch64, riscv64 |
| Copy-and-patch | partial (ALU ops) | template memcpy + sentinel patch, ISel fallback | x86_64 |
| Real LLVM | full | LLVM C API, obj/exe only | all LLVM targets |

ISel uses stack-based register allocation (every vreg gets a stack slot, computation through scratch registers) with fused instruction selection and binary encoding in one pass.

## Output Modes

| Mode | Flag | Formats |
|------|------|---------|
| Executable | default / `-o` (when `@main` exists) | ELF (x86_64, aarch64, riscv64), Mach-O (aarch64) |
| JIT | `--jit` | mmap'd code, W^X, dlsym symbol resolution |
| Object file | `-o` (when `@main` is absent) | ELF64, Mach-O |
| IR dump | `--dump-ir` | LLVM IR text |

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

## Platform Support

| Platform | JIT | Obj | Exe | Intrinsic blobs |
|----------|-----|-----|-----|-----------------|
| Linux x86_64 | yes | ELF | ELF (static/dynamic) | 60 |
| Linux aarch64 | yes | ELF | ELF (static) | 26 |
| Linux riscv64 | yes | -- | ELF (static) | -- |
| macOS aarch64 | yes | Mach-O | Mach-O + codesign | -- |

Intrinsic blobs are hand-written assembly for LLVM intrinsics (sqrt, exp, memcpy, etc.), embedded into JIT buffers and executables.

## Performance

Measured on February 13, 2026:

- CPU: AMD Ryzen 9 5950X 16-Core Processor
- OS: Linux 6.18.9-2-cachyos x86_64
- Compiler: GCC 15.2.1

### LLVM IR Corpus (repo `tests/ll`, 8 `@main` tests, 2.42 KiB IR)

Liric metrics are from `liric_probe_runner --timing` (`parse_us`, `compile_us`).
LLVM metrics are from `bench_lli_phases --json` (`parse_ms`, `jit_ms`).
`Total JIT` is `parse + compile`.

| Phase | liric (ms, sum) | LLVM ORC (ms, sum) | Speedup |
|-------|------------------|--------------------|---------|
| Parse | 0.601600 | 0.860941 | 1.43x |
| Compile | 0.384100 | 0.015428 | 0.04x |
| Total JIT | 0.985700 | 0.876369 | 0.89x |

Per-test distribution (nearest-rank):

| Metric (`parse+compile`) | liric (ms) | LLVM ORC (ms) | Speedup |
|--------------------------|------------|---------------|---------|
| p50 | 0.094900 | 0.098966 | 1.04x |
| p95 | 0.215900 | 0.182233 | 0.84x |
| slowest | 0.215900 | 0.182233 | 0.84x |

### Micro-benchmarks (`bench_tcc`, 5 cases, best-of-5)

| Metric | tcc | liric | Ratio |
|--------|-----|-------|-------|
| Wall-clock exe mode (us, sum) | 7329 | 22443 | 0.33x |
| In-process compile+reloc (us, sum) | 1033.2 | 48.1 | 21.5x |

### LFortran Integration Bench Commands

The canonical integration commands are:

```bash
./build/bench_compat_check --timeout 15
./build/bench_ll --iters 3
./build/bench_api --iters 3
./build/bench_corpus --iters 3
```

On this snapshot, `bench_compat_check --limit 50` produced `compat_ll.txt` with
0 tests (`liric_match=0` in that sample), so `bench_ll` and `bench_api` ran with
empty corpora (`Benchmarking 0 tests`). Artifacts were still written to
`/tmp/liric_bench/`.

### Reproduction

```bash
# 1) Bounded compat sweep (writes /tmp/liric_bench/*.txt and *.jsonl)
timeout 600 ./build/bench_compat_check --timeout 15 --limit 50

# 2) Canonical benchmark binaries
./build/bench_ll --iters 1
./build/bench_api --iters 1
./build/bench_tcc --iters 5
./build/bench_corpus --iters 1

# 3) Phase breakdown used for the table above (repo tests/ll corpus)
for f in tests/ll/*.ll; do
  rg -q 'define\\s+[^@]+@main\\s*\\(' "$f" || continue
  ./build/liric_probe_runner --timing --ignore-retcode --func main --sig i32 "$f"
  ./build/bench_lli_phases --json --iters 1 --func main --sig i32 "$f"
done
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
| Object/exe emission | `objfile.c`, `objfile_elf.c`, `objfile_macho.c` |
| Intrinsic stubs | `platform/*.S` |
| Session API | `session.c` |
| Compat API | `liric_compat.c` |
| LLVM backend | `llvm_backend.c` |
| CLI | `tools/liric_main.c` |

## License

MIT
