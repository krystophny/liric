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

## Benchmarking (C Harness)

Benchmark flow is fully C-based:

1. Discover compatibility set (`lfortran` native vs `liric` vs `lli`):
```bash
./build/bench_compat_check --timeout 15
```

2. Run LL benchmark on that compatibility set:
```bash
./build/bench_ll --iters 3
```

3. The fair in-process LLVM phase timer used by `bench_ll`:
```bash
./build/bench_lli_phases --json --iters 1 --sig i32_argc_argv /tmp/liric_bench/ll/<test>.ll
```

Outputs are written to `/tmp/liric_bench/`:
- `compat_check.jsonl`
- `compat_api.txt`
- `compat_ll.txt`
- `bench_ll.jsonl`

Latest run (February 8, 2026, CachyOS x86_64, `lfortran` LLVM backend, `lli -O0`):

- Compatibility sweep (`bench_compat_check`):
  - Processed: `2266`
  - `llvm_ok`: `2246` (99.1%)
  - `liric_match`: `1027` (45.3%)
  - `lli_match`: `2166` (95.6%)
  - `both_match` (`compat_ll`): `1014` (44.7%)

- LL benchmark (`bench_ll --iters 3`) on `compat_ll`:
  - Benchmarked files: `989`

| Metric | liric wall | lli wall | Wall speedup |
|--------|-----------:|---------:|-------------:|
| Median | 10.065 ms | 20.121 ms | **2.00x** |
| Aggregate | 10045 ms | 20855 ms | 2.08x |
| P90 / P95 | - | - | 2.00x / 3.00x |

| Metric | liric internal | lli internal | Internal speedup |
|--------|---------------:|-------------:|-----------------:|
| Median | 0.383600 ms | 0.226347 ms | **0.52x** |
| Aggregate | 659.689 ms | 312.038 ms | 0.47x |
| P90 / P95 | - | - | 0.66x / 0.70x |

The internal metric is now fair (in-process parse+compile vs in-process parse+compile).

## License

MIT
