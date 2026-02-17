# README Performance Snapshot

Generated: 2026-02-17T01:55:33Z
Benchmark commit: 4be7b53af19293797050d1dae784eb5cb16c0f0d
Host: AMD Ryzen 9 5950X 16-Core Processor (Linux 6.19.0-2-cachyos x86_64 GNU/Linux)
Toolchain: cc (GCC) 15.2.1 20260209; LLVM version 21.1.8
Dataset: corpus_100 (expected 100, attempted 100, iters 3)
Canonical track: corpus_canonical (OK; completed 100/100)

Artifacts:
- /tmp/liric_bench/bench_corpus_compare_summary.json
- /tmp/liric_bench/bench_corpus_compare.jsonl
- /home/ert/code/lfortran-dev/liric/docs/benchmarks/readme_perf_snapshot.json

Legend: canonical corpus lane only; no duplicate tracks.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| corpus_canonical (canonical) | 100/100 | 0.158 | 0.146 | 0.300 | 0.290 | 4.252 | 4.550 | 31.51x | 34.20x | 15.62x | 16.46x |
