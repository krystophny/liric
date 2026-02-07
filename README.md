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

1195 LFortran-generated `.ll` files, LLVM 21.1.6.

| Metric | liric | lli -O0 | Speedup |
|--------|------:|--------:|--------:|
| Median | 1.23 ms | 12.35 ms | **10.0x** |
| Mean | 1.78 ms | 14.72 ms | **8.3x** |
| P90 | 2.77 ms | 20.56 ms | 7.4x |

Total: 2.1s (liric) vs 17.6s (lli). 100% of tests faster, 45% over 10x.

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
| **Pass** | 1207 | 50.0% |
| JIT fail | 395 | 16.4% |
| Unsupported feature | 350 | 14.5% |
| Parse fail | 192 | 7.9% |
| Unsupported ABI | 150 | 6.2% |
| Mismatch | 120 | 5.0% |

## License

MIT
