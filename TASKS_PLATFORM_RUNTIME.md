# Platform Runtime / Direct Binary Follow-ups

## Done in this branch

- [x] Re-introduce direct executable intrinsic embedding on Linux x86_64.
- [x] Add Linux arm64 direct executable emission (`ET_EXEC`) with native relocation patching.
- [x] Add Linux arm64 intrinsic stub blob set for math/memory intrinsics used by liric JIT builtins.
- [x] Fix ELF object relocation mapping for aarch64 (`elf_reloc_aarch64` instead of x86 mapper).
- [x] Extend CI build+test matrix to Linux x86_64, Linux arm64, and macOS arm64.
- [x] Add CI platform-shape checks for Linux/macOS/Windows x86_64+arm64 and Linux riscv64 macro surfaces.

## Remaining follow-up PRs

- [ ] Add native executable emitters for macOS (Mach-O runnable binary path) and Windows (PE runnable binary path).
- [ ] Add backend support for riscv64 (`target_riscv64.c`) so Linux riscv64 is fully runnable end-to-end.
- [ ] Add Linux riscv64 intrinsic blobs once riscv64 backend/executable path is in place.
- [ ] Add native runtime tests on Windows runners (x86_64 + arm64 when available) for direct executable mode.
- [ ] Add native runtime tests on macOS for direct executable mode once Mach-O executable emission lands.
