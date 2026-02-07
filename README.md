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

## Parse Overhead Analysis

Across 520 LFortran-generated `.ll` files, text parsing accounts for
a significant fraction of total compile time. The C builder API eliminates
this overhead entirely.

| Metric | Parse | JIT | Total | Parse % |
|--------|------:|----:|------:|--------:|
| Median | 0.091 ms | 0.138 ms | 0.234 ms | 42.2% |
| Mean | 0.194 ms | 0.286 ms | 0.480 ms | 46.5% |
| P90 | 0.394 ms | 0.604 ms | 0.948 ms | 79.6% |
| P95 | 0.562 ms | 0.801 ms | 1.395 ms | 81.2% |

- Aggregate parse fraction: 40.5% of total compile time
- 57% of tests have >=40% parse overhead
- 13% of tests have >=70% parse overhead (builder API gives 3-9x speedup)

```bash
python3 -m tools.bench_parse_overhead   # reproduce
```

## Speed: liric vs LLVM lli

513 LFortran-generated `.ll` files, macOS arm64, LLVM 21.1.7.

| Metric | liric | lli -O0 | Speedup |
|--------|------:|--------:|--------:|
| Median | 5.34 ms | 17.67 ms | **3.3x** |
| Mean | 5.82 ms | 18.70 ms | **3.2x** |
| P90 | 6.20 ms | 21.07 ms | 3.4x |

Total: 2.99s (liric) vs 9.60s (lli). 99.8% of tests faster.
Wall-clock times include process startup (~4ms); in-process JIT compile
is sub-millisecond (see parse overhead above).

```bash
python3 -m tools.bench_compile_speed   # reproduce
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
