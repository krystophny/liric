# README Performance Snapshot

Generated: 2026-02-14T23:43:56Z
Benchmark commit: b1f2bf5444341da9ea89c0df12576276c0db0b8e
Host: AMD Ryzen 9 5950X 16-Core Processor (Linux 6.19.0-2-cachyos x86_64 GNU/Linux)
Toolchain: cc (GCC) 15.2.1 20260209; LLVM version 21.1.6
Dataset: corpus_100 (expected 100, attempted 100, iters 3)
Canonical track: runtime_equalized_bc (complete; completed 100/100)

Artifacts:
- /tmp/liric_bench/bench_corpus_compare_summary.json
- /tmp/liric_bench/bench_corpus_compare_core.jsonl
- /tmp/liric_bench/bench_corpus_compare_runtime_equalized_bc.jsonl
- /home/ert/code/lfortran-dev/liric/docs/benchmarks/readme_perf_snapshot.json

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|-----------------------:|--------------------------:|
| core | 100/100 | 0.171 | 0.162 | 0.317 | 0.288 | 4.405 | 4.708 | 14.48x | 18.62x |
| runtime_equalized_bc (canonical) | 100/100 | 0.180 | 21.300 | 21.552 | 10.820 | 361.866 | 372.507 | 17.28x | 6.62x |
