# README Performance Snapshot

Generated: 2026-02-17T23:08:10Z
Benchmark commit: 50ba263db7c46c59c93216c00d836324cf46c83b
Host: AMD Ryzen 9 5950X 16-Core Processor (Linux 6.19.2-2-cachyos x86_64 GNU/Linux)
Toolchain: cc (GCC) 15.2.1 20260209; LLVM version 21.1.8
Dataset: corpus_100 (expected 100, attempted 95, iters 3)
Canonical track: corpus_canonical (OK; completed 95/95)

Artifacts:
- /tmp/liric_bench/bench_corpus_compare_summary.json
- /tmp/liric_bench/bench_corpus_compare.jsonl
- /home/ert/code/lfortran-dev/liric/docs/benchmarks/readme_perf_snapshot.json

Legend: canonical corpus lane only; no duplicate tracks.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| corpus_canonical (canonical) | 95/95 | 0.225 | 0.084 | 0.304 | 0.336 | 5.216 | 5.648 | 63.18x | 51.02x | 17.46x | 16.06x |
