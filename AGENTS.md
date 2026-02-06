# AGENTS.md

## Purpose
Liric is a compact LLVM IR subset parser + JIT compiler.
Core flow: parse LLVM-like text -> IR -> machine-code backend -> executable function pointers.

## Fast Repo Map
- `CMakeLists.txt`: build graph for library, CLI, and tests
- `include/liric/liric.h`: public C API
- `src/ir.*`: IR types, constructors, and dumping
- `src/ll_lexer.*`: lexer for .ll subset
- `src/ll_parser.*`: parser lowering text to IR
- `src/target.h`: backend interface
- `src/target_registry.c`: target lookup + host detection
- `src/target_x86_64.*`: x86_64 backend
- `src/target_aarch64.*`: aarch64 backend
- `src/jit.*`: executable memory + module compilation + symbol resolution
- `tools/liric_main.c`: CLI entrypoint
- `tests/*.c`: unit/integration/e2e test suite

## Local Build And Test
- Configure: `cmake -S . -B build -G Ninja`
- Build: `cmake --build build -j32`
- Run tests: `ctest --test-dir build --output-on-failure`

## Platform Targets
- Supported execution hosts:
- Linux x86_64
- macOS arm64
- Default JIT target is host architecture.
- Explicit non-host target requests fail fast in JIT creation.

## Where To Change What
- Add/modify backend behavior: `src/target_*.c` plus `src/target.h`
- Change target selection policy: `src/target_registry.c` and `src/jit.c`
- Change public API: `include/liric/liric.h` and mirrored declarations in `src/jit.h`
- Add tests: `tests/` and register in `tests/test_main.c` + `CMakeLists.txt`

## Debugging Shortcuts
- If parser issues: start in `src/ll_lexer.c` then `src/ll_parser.c`
- If JIT crashes: inspect emitted backend code path in `src/target_*.c` and executable memory handling in `src/jit.c`
- If symbol lookup fails: inspect `dlsym` path in `src/jit.c`

## Done Criteria For Backend Work
- Build succeeds with `-Werror`
- All tests pass via CTest
- JIT tests execute correctly on host architecture
- New/changed API is covered by tests
