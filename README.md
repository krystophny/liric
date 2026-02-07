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
./build/liric --jit file.ll
./build/liric --dump-ir file.ll
./build/liric --jit file.wasm --func add --args 2 3
```

Linux x86_64 and macOS arm64.

## Speed: liric vs LLVM lli

1095 LFortran-generated `.ll` files, LLVM 21.1.6. `-O0` and `-O2` are
nearly identical because LLVM JIT startup overhead dominates.

| Metric | liric | lli -O0 | Speedup | lli -O2 | Speedup |
|--------|------:|--------:|--------:|--------:|--------:|
| Median | 1.35 ms | 12.87 ms | **9.5x** | 12.84 ms | **9.5x** |
| Mean | 2.46 ms | 15.03 ms | **6.1x** | 15.01 ms | **6.1x** |
| P90 | 4.12 ms | 20.37 ms | 4.9x | 19.96 ms | 4.8x |

Total: 2.7s (liric) vs 16.5s (lli). 100% of tests faster, 38% over 10x.

```bash
python3 -m tools.bench_compile_speed   # reproduce
```

## LFortran Test Suite

```bash
python3 -m tools.lfortran_mass.run_mass --workers $(nproc)
```

| Classification | Count | % |
|---------------|------:|--:|
| **Pass** | 1107 | 45.8% |
| JIT fail | 506 | 20.9% |
| Unsupported feature | 351 | 14.5% |
| Parse fail | 192 | 7.9% |
| Unsupported ABI | 150 | 6.2% |
| Mismatch | 108 | 4.5% |

## License

MIT
