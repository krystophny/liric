# README Performance Snapshot

Generated: 2026-02-14T17:35:55Z
Benchmark commit: 3585727865731128abfc078468de7fa8fcdfc20b
Host: AMD Ryzen 9 5950X 16-Core Processor (Linux 6.19.0-2-cachyos x86_64 GNU/Linux)
Toolchain: cc (GCC) 15.2.1 20260209; LLVM version 21.1.6
Dataset: 1 tests from tests/ll, 1 iterations

Artifacts:
- /tmp/liric_bench_readme_smoke/compat_check.jsonl
- /tmp/liric_bench_readme_smoke/bench_ll.jsonl
- /tmp/liric_bench_readme_smoke/bench_ll_summary.json
- /home/ert/code/lfortran-dev/liric/docs/benchmarks/readme_perf_snapshot.json

| Phase | liric (ms) | LLVM ORC (ms) | Speedup |
|-------|-----------:|--------------:|--------:|
| Parse | 0.068 | 0.058 | 0.9x |
| Compile | 7.888 | 0.002 | 0.0x |
| Total (parse+compile) | 7.955 | 0.059 | 0.0x |
