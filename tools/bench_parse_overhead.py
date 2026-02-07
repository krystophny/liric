#!/usr/bin/env python3
"""Measure text-parse overhead across lfortran mass-test .ll files.

For each passing test, runs bench_parse_vs_jit to measure parse time vs JIT time.
The parse fraction represents the speedup achievable by the C builder API
(which eliminates text parsing entirely).

Usage:
    python3 -m tools.bench_parse_overhead [--results PATH] [--iters N] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path


def load_passing_ll_files(results_path: Path) -> list[str]:
    paths = []
    with open(results_path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("classification") == "pass" and r.get("jit_cmd"):
                for token in r["jit_cmd"].split():
                    if token.endswith(".ll"):
                        if Path(token).exists():
                            paths.append(token)
                        break
    return paths


def percentile(data: list[float], p: float) -> float:
    if not data:
        return 0.0
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def main():
    parser = argparse.ArgumentParser(description="Benchmark text-parse overhead")
    parser.add_argument("--results", default="/tmp/liric_lfortran_mass/results.jsonl")
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--output", default="/tmp/parse_overhead_results.md")
    parser.add_argument("--bench-bin", default=None,
                        help="Path to bench_parse_vs_jit binary")
    parser.add_argument("--workers", type=int, default=1)
    args = parser.parse_args()

    bench_bin = args.bench_bin
    if not bench_bin:
        candidates = [
            Path(__file__).resolve().parent.parent / "build" / "bench_parse_vs_jit",
        ]
        for c in candidates:
            if c.exists():
                bench_bin = str(c)
                break
    if not bench_bin or not Path(bench_bin).exists():
        print("ERROR: bench_parse_vs_jit not found. Build first.", file=sys.stderr)
        sys.exit(1)

    results_path = Path(args.results)
    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run mass tests first.", file=sys.stderr)
        sys.exit(1)

    ll_files = load_passing_ll_files(results_path)
    print(f"Found {len(ll_files)} passing .ll files")

    results = []
    errors = 0
    for i, ll_path in enumerate(ll_files):
        try:
            p = subprocess.run(
                [bench_bin, "--json", "--iters", str(args.iters), ll_path],
                capture_output=True, text=True, timeout=30
            )
            if p.returncode == 0 and p.stdout.strip():
                r = json.loads(p.stdout.strip())
                if "error" not in r:
                    results.append(r)
                else:
                    errors += 1
            else:
                errors += 1
        except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError):
            errors += 1

        if (i + 1) % 200 == 0:
            print(f"  {i+1}/{len(ll_files)} done, {len(results)} ok, {errors} errors")

    print(f"\nCompleted: {len(results)} measured, {errors} errors")

    if not results:
        print("ERROR: no results", file=sys.stderr)
        sys.exit(1)

    parse_times = [r["parse_ms"] for r in results]
    jit_times = [r["jit_ms"] for r in results]
    total_times = [r["total_ms"] for r in results]
    parse_pcts = [r["parse_pct"] for r in results]
    ll_sizes = [r["ll_bytes"] for r in results]

    lines = []
    lines.append("# Text-Parse Overhead Analysis")
    lines.append("")
    lines.append(f"- **Tests measured:** {len(results)}")
    lines.append(f"- **Iterations per test:** {args.iters}")
    lines.append(f"- **Total parse time:** {sum(parse_times):.1f} ms")
    lines.append(f"- **Total JIT time:** {sum(jit_times):.1f} ms")
    lines.append(f"- **Total time:** {sum(total_times):.1f} ms")
    overall_pct = 100.0 * sum(parse_times) / sum(total_times) if sum(total_times) > 0 else 0
    lines.append(f"- **Parse fraction (aggregate):** {overall_pct:.1f}%")
    speedup = sum(total_times) / sum(jit_times) if sum(jit_times) > 0 else 0
    lines.append(f"- **Max speedup from builder API:** {speedup:.1f}x "
                 "(eliminates parse, keeps JIT)")
    lines.append("")

    lines.append("## Per-Test Statistics (milliseconds)")
    lines.append("")
    lines.append("| Metric | Parse | JIT | Total | Parse % |")
    lines.append("|--------|------:|----:|------:|--------:|")
    for label, pfn in [
        ("Median", statistics.median),
        ("Mean", statistics.mean),
        ("P25", lambda d: percentile(d, 25)),
        ("P75", lambda d: percentile(d, 75)),
        ("P90", lambda d: percentile(d, 90)),
        ("P95", lambda d: percentile(d, 95)),
        ("Min", min),
        ("Max", max),
    ]:
        lines.append(
            f"| {label} | {pfn(parse_times):.3f} | {pfn(jit_times):.3f} | "
            f"{pfn(total_times):.3f} | {pfn(parse_pcts):.1f}% |"
        )
    lines.append("")

    lines.append("## Parse Fraction Distribution")
    lines.append("")
    thresholds = [90, 80, 70, 60, 50, 40, 30]
    for t in thresholds:
        count = sum(1 for p in parse_pcts if p >= t)
        pct = 100.0 * count / len(parse_pcts)
        lines.append(f"- >={t}% parse overhead: {count} tests ({pct:.1f}%)")
    lines.append("")

    lines.append("## Top 10 Highest Parse Overhead")
    lines.append("")
    lines.append("| File | Size | Parse ms | JIT ms | Total ms | Parse % |")
    lines.append("|------|-----:|---------:|-------:|---------:|--------:|")
    sorted_by_pct = sorted(results, key=lambda x: -x["parse_pct"])
    for r in sorted_by_pct[:10]:
        fn = Path(r["file"]).name
        lines.append(
            f"| {fn} | {r['ll_bytes']:,} | {r['parse_ms']:.3f} | "
            f"{r['jit_ms']:.3f} | {r['total_ms']:.3f} | {r['parse_pct']:.1f}% |"
        )
    lines.append("")

    lines.append("## Top 10 Lowest Parse Overhead")
    lines.append("")
    lines.append("| File | Size | Parse ms | JIT ms | Total ms | Parse % |")
    lines.append("|------|-----:|---------:|-------:|---------:|--------:|")
    sorted_by_pct_asc = sorted(results, key=lambda x: x["parse_pct"])
    for r in sorted_by_pct_asc[:10]:
        fn = Path(r["file"]).name
        lines.append(
            f"| {fn} | {r['ll_bytes']:,} | {r['parse_ms']:.3f} | "
            f"{r['jit_ms']:.3f} | {r['total_ms']:.3f} | {r['parse_pct']:.1f}% |"
        )
    lines.append("")

    report = "\n".join(lines)
    Path(args.output).write_text(report)
    print(f"\nReport written to {args.output}")
    print(report)


if __name__ == "__main__":
    main()
