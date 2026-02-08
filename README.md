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

Both harnesses run executed test binaries in isolated temporary working
directories under `/tmp/liric_bench/` to avoid polluting the repository root
with runtime-generated files.

3. The fair in-process LLVM phase timer used by `bench_ll`:
```bash
./build/bench_lli_phases --json --iters 1 --sig i32_argc_argv /tmp/liric_bench/ll/<test>.ll
```

Outputs are written to `/tmp/liric_bench/`:
- `compat_check.jsonl`
- `compat_api.txt`
- `compat_ll.txt`
- `bench_ll.jsonl`
- `bench_ll_summary.json`

Latest run (February 8, 2026, CachyOS x86_64, `lfortran` LLVM backend, `lli -O0`):

- Compatibility sweep (`bench_compat_check`):
  - Processed: `2266`
  - `llvm_ok`: `2246` (99.1%)
  - `liric_match`: `1025` (45.2%)
  - `lli_match`: `2162` (95.4%)
  - `both_match` (`compat_ll`): `1013` (44.7%)

- LL benchmark (`bench_ll --iters 3`) on `compat_ll`:
  - Benchmarked files: `988`

| Metric | liric wall | lli wall | Wall speedup |
|--------|-----------:|---------:|-------------:|
| Median | 10.064 ms | 20.120 ms | **2.00x** |
| Aggregate | 10024 ms | 20606 ms | 2.06x |
| P90 / P95 | - | - | 2.00x / 3.00x |

| Metric | liric parse+compile | lli parse+jit | Legacy speedup |
|--------|---------------:|-------------:|-----------------:|
| Median | 0.368150 ms | 0.223440 ms | **0.54x** |
| Aggregate | 590.492 ms | 309.075 ms | 0.52x |
| P90 / P95 | - | - | 0.66x / 0.70x |

| Internal Split (median) | liric | lli |
|-------------------------|------:|----:|
| parse | 0.315400 ms | 0.220214 ms |
| compile/jit | 0.054450 ms | 0.003066 ms |
| lookup/materialization | n/a | 3.331356 ms |

`bench_ll` now uses materialization-fair internal speedup as the primary metric:
`liric` parse+compile+lookup vs `LLVM` parse+jit+lookup. The parse+compile vs
parse+jit value is still emitted as a legacy metric for diagnostic use.

## License

MIT
