# README Performance Snapshot

Generated: 2026-02-21T00:18:56Z
Benchmark commit: d70f02c58571cad57e30165a55f67d3e28d9e6a6
Host: Apple M3 Ultra (Darwin 25.2.0 arm64)
Toolchain: gcc-15 (Homebrew GCC 15.2.0) 15.2.0; unavailable
Dataset: corpus_100 (expected 100, attempted 97)
Canonical track: corpus_canonical (OK; completed 97/97)

Artifacts:
- /private/tmp/liric_bench/bench_corpus_compare_summary.json
- /private/tmp/liric_bench/bench_corpus_compare.jsonl
- /Users/ert/code/liric/docs/benchmarks/readme_perf_snapshot.json

Legend: canonical corpus lane only; no duplicate tracks.

| Track | Completed | liric parse (ms) | liric compile+lookup (ms) | liric total materialized (ms) | LLVM parse (ms) | LLVM add+lookup (ms) | LLVM total materialized (ms) | Speedup non-parse (median) | Speedup non-parse (aggregate) | Speedup total (median) | Speedup total (aggregate) |
|-------|----------:|-----------------:|--------------------------:|------------------------------:|----------------:|---------------------:|-----------------------------:|----------------------------:|-------------------------------:|-----------------------:|--------------------------:|
| corpus_canonical (canonical) | 97/97 | 0.118 | 0.065 | 0.192 | 0.235 | 3.967 | 4.210 | 59.63x | 9.63x | 22.73x | 7.48x |
