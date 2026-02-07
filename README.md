# Liric

Liric (Lightweight IR Compiler) is a fast, minimal JIT compiler with two
frontends: LLVM IR text (`.ll`) and WebAssembly binary (`.wasm`). Written in
C11 with zero external dependencies. Designed for low-latency JIT where full
LLVM overhead is unacceptable.

## Pipeline

```
LLVM IR text (.ll)                    WASM binary (.wasm)
    |                                      |
    v                                      v
ll_lexer --> ll_parser               wasm_decode --> wasm_to_ir
 (tokens)    (IR AST)                 (sections)    (stack->SSA)
    |                                      |
    +-------------- lr_module_t -----------+
                  (target-independent SSA IR)
                         |
                         v
                   ISel --> encode --> mmap+exec
                   (MIR)   (binary)   (JIT run)
```

The CLI auto-detects format by checking the first 4 bytes for WASM magic (`\0asm`).

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Clean build takes ~110ms. No external dependencies beyond a C compiler and CMake.

## Usage

```bash
# JIT compile and run an LLVM IR file
./build/liric_cli --jit tests/ll/ret42.ll

# Dump parsed IR
./build/liric_cli --dump-ir tests/ll/ret42.ll

# JIT a WASM file
./build/liric_cli --jit tests/wasm/add.wasm --func add --args 2 3
```

## Platform Support

| Platform | Status |
|----------|--------|
| Linux x86_64 | Full support |
| macOS arm64 | Full support (MAP_JIT) |

## Compile Speed: liric vs LLVM lli

Benchmarked on 1096 LFortran-generated `.ll` files (matched pairs where both
tools succeed). LLVM version 21.1.6.

| Metric | liric | lli | Speedup |
|--------|------:|----:|--------:|
| Median | 1.34 ms | 12.95 ms | **9.7x** |
| Mean | 2.45 ms | 15.23 ms | **6.2x** |
| P25 | 1.09 ms | 11.76 ms | 10.8x |
| P75 | 2.03 ms | 15.32 ms | 7.6x |
| P90 | 4.18 ms | 20.88 ms | 5.0x |
| P95 | 7.75 ms | 30.36 ms | 3.9x |
| P99 | 16.92 ms | 43.96 ms | 2.6x |

- **Total wall-clock:** 2.7s (liric) vs 16.7s (lli) = **6.2x overall**
- **100%** of tests faster with liric, **42.5%** over 10x faster
- `.ll` files range from 494 bytes to 1MB

Reproduce with:
```bash
python3 -m tools.bench_compile_speed
```

## Testing with LFortran

Liric includes a mass testing harness that compiles all LFortran integration
tests through the pipeline: LFortran emits LLVM IR, liric JIT-compiles and
runs it, and results are compared against reference output.

### Prerequisites

- [LFortran](https://lfortran.org/) built with LLVM support
  (expected at `../lfortran/build/`)

### Run mass tests

```bash
python3 -m tools.lfortran_mass.run_mass --workers $(nproc)
```

Results are written to `/tmp/liric_lfortran_mass/`. Key output files:
- `summary.md` -- overall statistics
- `results.jsonl` -- per-test details
- `failures.csv` -- all non-passing tests with classification

### Current Results (2415 tests)

| Classification | Count | % |
|---------------|------:|--:|
| **Pass** | 1107 | 45.8% |
| JIT fail | 506 | 20.9% |
| Unsupported feature | 351 | 14.5% |
| Parse fail | 192 | 7.9% |
| Unsupported ABI | 150 | 6.2% |
| Mismatch | 108 | 4.5% |
| LFortran emit fail | 1 | 0.0% |

## License

MIT
