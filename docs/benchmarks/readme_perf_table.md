# README Performance Snapshot

Generated: 2026-02-21T12:35:16Z
Benchmark commit: 2d1b689f24951af124309315c774e88963fdc006
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
| corpus_canonical (canonical) | 95/95 | 0.154 | 0.150 | 0.305 | 0.205 | 3.333 | 3.501 | 21.79x | 30.05x | 11.38x | 12.56x |
