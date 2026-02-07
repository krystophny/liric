# Liric

Fast, minimal JIT compiler for LLVM IR (`.ll`) and WebAssembly (`.wasm`).
C11, zero dependencies. ~110ms clean build.

```
.ll / .wasm  -->  parse  -->  IR (SSA)  -->  ISel (MIR)  -->  encode  -->  JIT
```

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Usage

```bash
# Text frontend (parse .ll or .wasm, then JIT)
./build/liric --jit file.ll
./build/liric --dump-ir file.ll
./build/liric --jit file.wasm --func add --args 2 3

# C builder API (construct IR programmatically, skip parsing)
#include <liric/liric.h>
# See include/liric/liric.h for full API reference
```

Linux x86_64 and macOS arm64.

## C Builder API

The builder API lets callers construct IR directly in C, eliminating the
text-parse overhead entirely. This is the recommended interface for compiler
backends (like LFortran) that generate IR programmatically.

```c
#include <liric/liric.h>

lr_module_t *m = lr_module_create_new();
lr_type_t *i32 = lr_type_i32_get(m);
lr_func_t *f = lr_func_define(m, "add", i32, (lr_type_t*[]){i32, i32}, 2, false);
uint32_t a = lr_func_param_vreg(f, 0), b = lr_func_param_vreg(f, 1);
lr_block_t *entry = lr_block_new(f, m, "entry");
uint32_t c = lr_build_add(m, entry, f, i32, LR_VREG(a, i32), LR_VREG(b, i32));
lr_build_ret(m, entry, LR_VREG(c, i32));

lr_jit_t *jit = lr_jit_create();
lr_jit_add_module(jit, m);
int (*fn)(int, int) = lr_jit_get_function(jit, "add");
printf("%d\n", fn(10, 32));  // 42
```

Full API: `include/liric/liric.h` — types, functions, blocks, 40+ instruction
builders, globals, PHI, GEP, calls, type conversions, JIT lifecycle.

## Speed: liric vs LLVM ORC JIT

514 LFortran-generated `.ll` files, in-process measurement, macOS arm64,
LLVM 21.1.7, 100 iterations per file. Both compilers load the same runtime
library. Only files passing both compilers are counted.

| Metric | liric | LLVM ORC | Speedup |
|--------|------:|---------:|--------:|
| Median | 0.067 ms | 1.382 ms | **23.3x** |
| Mean | 0.117 ms | 2.363 ms | **26.2x** |
| P90 | 0.242 ms | 4.556 ms | **42.5x** |
| P95 | 0.316 ms | 5.716 ms | 50.7x |

Aggregate: 60 ms (liric) vs 1214 ms (LLVM). 99.8% of tests faster.

JIT compile only (no text parsing): **75.4x** median speedup.

```bash
python3 -m tools.bench_h2h --workers $(nproc)   # reproduce
```

## End-to-End: LFortran + liric vs LFortran + LLVM

493 Fortran tests passing both backends, macOS arm64, 3 iterations each.
Measures total wall-clock time from Fortran source to program output.

- **liric path:** `lfortran --show-llvm` → `liric_probe_runner` (JIT)
- **LLVM path:** `lfortran` (compile + link + run natively)

| Metric | liric path | LLVM native | Speedup |
|--------|----------:|-----------:|--------:|
| Median | 156 ms | 3093 ms | **19.8x** |
| Mean | 162 ms | 3065 ms | **18.9x** |
| P90 | 161 ms | 3162 ms | **20.4x** |

100% of tests faster. liric JIT takes 3.6 ms median; the rest is
`lfortran --show-llvm` IR emission (153 ms).

```bash
python3 -m tools.bench_lfortran_e2e --workers $(nproc)   # reproduce
```

## LFortran Test Suite

```bash
python3 -m tools.lfortran_mass.run_mass --workers $(nproc)
```

Latest snapshot (February 7, 2026):

| Classification | Count | % |
|---------------|------:|--:|
| **Pass** | 520 | 21.7% |
| JIT fail | 932 | 38.8% |
| Unsupported feature | 478 | 19.9% |
| Mismatch | 318 | 13.3% |
| Unsupported ABI | 151 | 6.3% |

## License

MIT
