# Liric Architecture Overview

Liric is a lightweight compiler/JIT runtime centered around a target-independent SSA IR.

Primary flow:

1. Frontends ingest LLVM IR text (`.ll`) or WASM binaries (`.wasm`).
2. Frontends construct `lr_module_t` using arena-owned IR nodes.
3. Target backends lower IR for host architecture (`x86_64` or `aarch64`).
4. Output is either:
   - in-memory executable code via JIT runtime, or
   - relocatable object files (ELF/Mach-O).

Compatibility layers expose LLVM-like C/C++ APIs while preserving liric internals.
Bench tooling compares liric against LLVM baselines for correctness and performance.
