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

Benchmarked on 1095 LFortran-generated `.ll` files (matched pairs where both
tools succeed). LLVM version 21.1.6. Note: `lli -O0` and `lli -O2` show
nearly identical times because JIT startup overhead dominates at these file
sizes.

| Metric | liric | lli -O0 | Speedup | lli -O2 | Speedup |
|--------|------:|--------:|--------:|--------:|--------:|
| Median | 1.35 ms | 12.87 ms | **9.5x** | 12.84 ms | **9.5x** |
| Mean | 2.46 ms | 15.03 ms | **6.1x** | 15.01 ms | **6.1x** |
| P25 | 1.11 ms | 11.65 ms | 10.5x | 11.59 ms | 10.5x |
| P75 | 2.11 ms | 15.20 ms | 7.2x | 15.20 ms | 7.2x |
| P90 | 4.12 ms | 20.37 ms | 4.9x | 19.96 ms | 4.8x |
| P95 | 7.77 ms | 29.13 ms | 3.7x | 28.38 ms | 3.7x |
| P99 | 17.32 ms | 43.40 ms | 2.5x | 45.20 ms | 2.6x |

- **Total wall-clock:** 2.7s (liric) vs 16.5s (lli -O0) vs 16.4s (lli -O2)
- **100%** of tests faster with liric, **37.8%** over 10x faster
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
