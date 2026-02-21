# README Performance Snapshot

Generated: 2026-02-21T14:32:08Z
Benchmark commit: 14389048ca0e171d9849256bd07c1ce92a1f2db9
Host: AMD Ryzen 9 5950X 16-Core Processor (Linux 6.19.3-2-cachyos x86_64 GNU/Linux)
Toolchain: cc (GCC) 15.2.1 20260209; LLVM version 21.1.8
Dataset: corpus_100 (expected 100, attempted 95)
Canonical track: corpus_canonical (OK; completed 95/95)

Artifacts:
- /tmp/liric_bench/bench_corpus_compare_summary.json
- /tmp/liric_bench/bench_corpus_compare.jsonl
- /home/ert/code/liric/docs/benchmarks/readme_perf_snapshot.json

Legend: canonical corpus lane only; no duplicate tracks.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| corpus_canonical (canonical) | 95/95 | 0.145 | 0.147 | 0.290 | 0.206 | 3.413 | 3.648 | 24.25x | 31.19x | 12.21x | 12.94x |
