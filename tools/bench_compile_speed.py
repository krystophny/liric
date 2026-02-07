#!/usr/bin/env python3
"""Benchmark liric JIT compile+run speed vs LLVM lli on lfortran-generated .ll files.

Reads results from a prior mass test run, selects passing tests, and times
both liric and lli on the same .ll files. Produces a markdown report.

Usage:
    python3 -m tools.bench_compile_speed [--results PATH] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class BenchResult:
    filename: str
    ll_path: str
    ll_bytes: int
    ll_lines: int
    liric_time: float
    lli_time: float
    speedup: float


def find_lli() -> Optional[str]:
    for name in ["lli", "lli-21", "lli-19", "lli-18", "lli-17", "lli-16", "lli-15",
                 "lli-14", "lli-13", "lli-12", "lli-11"]:
        path = shutil.which(name)
        if path:
            return path
    return None


def find_runtime_lib() -> Optional[str]:
    candidates = [
        Path(__file__).resolve().parent.parent.parent
        / "lfortran/build/src/runtime/liblfortran_runtime.so",
    ]
    for p in sorted(
        Path("/home").glob("*/code/lfortran*/lfortran/build/src/runtime/liblfortran_runtime.so*")
    ):
        candidates.append(p)

    for c in candidates:
        if c.exists():
            return str(c)

    for d in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        for f in Path(d).glob("liblfortran_runtime.so*") if Path(d).is_dir() else []:
            return str(f)
    return None


def load_passing_tests(results_path: Path) -> list[dict]:
    tests = []
    with open(results_path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("classification") == "pass" and r.get("jit_cmd"):
                ll_path = None
                for token in r["jit_cmd"].split():
                    if token.endswith(".ll"):
                        ll_path = token
                        break
                if ll_path and Path(ll_path).exists():
                    r["_ll_path"] = ll_path
                    tests.append(r)
    return tests


def bench_one(
    test: dict,
    liric_cli: str,
    lli: str,
    runtime_lib: Optional[str],
    timeout: float = 30.0,
) -> Optional[BenchResult]:
    ll_path = test["_ll_path"]
    filename = test.get("filename", Path(ll_path).name)

    ll_stat = Path(ll_path).stat()
    ll_bytes = ll_stat.st_size
    with open(ll_path) as f:
        ll_lines = sum(1 for _ in f)

    jit_cmd = test["jit_cmd"].split()

    env = os.environ.copy()
    if runtime_lib:
        rt_dir = str(Path(runtime_lib).parent)
        env["LD_LIBRARY_PATH"] = rt_dir + ":" + env.get("LD_LIBRARY_PATH", "")

    try:
        t0 = time.monotonic()
        p = subprocess.run(jit_cmd, capture_output=True, timeout=timeout, env=env)
        liric_time = time.monotonic() - t0
        if p.returncode != 0:
            return None
    except (subprocess.TimeoutExpired, OSError):
        return None

    lli_cmd = [lli]
    if runtime_lib:
        lli_cmd += ["-load", runtime_lib]
    lli_cmd.append(ll_path)

    try:
        t0 = time.monotonic()
        p = subprocess.run(lli_cmd, capture_output=True, timeout=timeout, env=env)
        lli_time = time.monotonic() - t0
        if p.returncode != 0:
            return None
    except (subprocess.TimeoutExpired, OSError):
        return None

    speedup = lli_time / liric_time if liric_time > 0 else float("inf")
    return BenchResult(
        filename=filename,
        ll_path=ll_path,
        ll_bytes=ll_bytes,
        ll_lines=ll_lines,
        liric_time=liric_time,
        lli_time=lli_time,
        speedup=speedup,
    )


def percentile(data: List[float], p: float) -> float:
    if not data:
        return 0.0
    data_sorted = sorted(data)
    k = (len(data_sorted) - 1) * p / 100.0
    f = int(k)
    c = f + 1
    if c >= len(data_sorted):
        return data_sorted[f]
    return data_sorted[f] + (k - f) * (data_sorted[c] - data_sorted[f])


def generate_report(results: List[BenchResult], lli_version: str) -> str:
    liric_times = [r.liric_time * 1000 for r in results]
    lli_times = [r.lli_time * 1000 for r in results]
    speedups = [r.speedup for r in results]
    ll_sizes = [r.ll_bytes for r in results]

    total_liric = sum(liric_times)
    total_lli = sum(lli_times)

    lines = []
    lines.append("# Compile Speed Benchmark: liric vs lli")
    lines.append("")
    lines.append(f"- **Tests benchmarked:** {len(results)} (matched pairs, both succeed)")
    lines.append(f"- **LLVM version:** {lli_version}")
    lines.append(f"- **Total wall-clock (liric):** {total_liric:.1f} ms")
    lines.append(f"- **Total wall-clock (lli):** {total_lli:.1f} ms")
    lines.append(f"- **Overall speedup:** {total_lli / total_liric:.1f}x")
    lines.append("")

    lines.append("## Aggregate Statistics (milliseconds)")
    lines.append("")
    lines.append("| Metric | liric | lli | Speedup |")
    lines.append("|--------|------:|----:|--------:|")
    for label, pfn in [
        ("Median", lambda d: statistics.median(d)),
        ("Mean", lambda d: statistics.mean(d)),
        ("P25", lambda d: percentile(d, 25)),
        ("P75", lambda d: percentile(d, 75)),
        ("P90", lambda d: percentile(d, 90)),
        ("P95", lambda d: percentile(d, 95)),
        ("P99", lambda d: percentile(d, 99)),
        ("Min", lambda d: min(d)),
        ("Max", lambda d: max(d)),
    ]:
        lv = pfn(liric_times)
        rv = pfn(lli_times)
        sp = rv / lv if lv > 0 else float("inf")
        lines.append(f"| {label} | {lv:.2f} | {rv:.2f} | {sp:.1f}x |")
    lines.append("")

    lines.append("## .ll File Size Distribution")
    lines.append("")
    lines.append(f"- Min: {min(ll_sizes):,} bytes")
    lines.append(f"- Median: {statistics.median(ll_sizes):,.0f} bytes")
    lines.append(f"- Mean: {statistics.mean(ll_sizes):,.0f} bytes")
    lines.append(f"- Max: {max(ll_sizes):,} bytes")
    lines.append("")

    lines.append("## Speedup Distribution")
    lines.append("")
    thresholds = [100, 50, 20, 10, 5, 2, 1]
    for t in thresholds:
        count = sum(1 for s in speedups if s >= t)
        pct = 100.0 * count / len(speedups)
        lines.append(f"- >={t}x faster: {count} tests ({pct:.1f}%)")
    lines.append("")

    lines.append("## Top 10 Largest Speedups")
    lines.append("")
    lines.append("| File | .ll size | liric (ms) | lli (ms) | Speedup |")
    lines.append("|------|:--------:|:----------:|:--------:|--------:|")
    for r in sorted(results, key=lambda x: -x.speedup)[:10]:
        lines.append(
            f"| {r.filename} | {r.ll_bytes:,} | {r.liric_time*1000:.1f} | "
            f"{r.lli_time*1000:.1f} | {r.speedup:.1f}x |"
        )
    lines.append("")

    lines.append("## Top 10 Smallest Speedups")
    lines.append("")
    lines.append("| File | .ll size | liric (ms) | lli (ms) | Speedup |")
    lines.append("|------|:--------:|:----------:|:--------:|--------:|")
    for r in sorted(results, key=lambda x: x.speedup)[:10]:
        lines.append(
            f"| {r.filename} | {r.ll_bytes:,} | {r.liric_time*1000:.1f} | "
            f"{r.lli_time*1000:.1f} | {r.speedup:.1f}x |"
        )
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Benchmark liric vs lli compile speed")
    parser.add_argument(
        "--results",
        default="/tmp/liric_lfortran_mass/results.jsonl",
        help="Path to mass test results.jsonl",
    )
    parser.add_argument(
        "--output",
        default="/tmp/compile_speed_results.md",
        help="Output markdown report path",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Per-test timeout in seconds (default: 30)",
    )
    args = parser.parse_args()

    liric_cli = str(
        Path(__file__).resolve().parent.parent / "build" / "liric_cli"
    )
    if not Path(liric_cli).exists():
        print(f"ERROR: liric_cli not found at {liric_cli}", file=sys.stderr)
        sys.exit(1)

    lli = find_lli()
    if not lli:
        print("ERROR: lli not found on PATH", file=sys.stderr)
        sys.exit(1)

    lli_version = "unknown"
    try:
        p = subprocess.run([lli, "--version"], capture_output=True, text=True, timeout=5)
        for line in p.stdout.splitlines():
            if "LLVM version" in line:
                lli_version = line.strip()
                break
    except Exception:
        pass

    runtime_lib = find_runtime_lib()
    print(f"liric_cli: {liric_cli}")
    print(f"lli:       {lli} ({lli_version})")
    print(f"runtime:   {runtime_lib}")
    print()

    results_path = Path(args.results)
    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run mass tests first.", file=sys.stderr)
        sys.exit(1)

    tests = load_passing_tests(results_path)
    print(f"Found {len(tests)} passing tests with .ll files")

    bench_results: List[BenchResult] = []
    skipped = 0
    for i, test in enumerate(tests):
        r = bench_one(test, liric_cli, lli, runtime_lib, timeout=args.timeout)
        if r:
            bench_results.append(r)
        else:
            skipped += 1
        if (i + 1) % 100 == 0:
            print(f"  {i+1}/{len(tests)} done, {len(bench_results)} matched, {skipped} skipped")

    print(f"\nBenchmarked {len(bench_results)} matched pairs ({skipped} skipped)")

    if not bench_results:
        print("ERROR: no matched pairs found", file=sys.stderr)
        sys.exit(1)

    report = generate_report(bench_results, lli_version)
    Path(args.output).write_text(report)
    print(f"Report written to {args.output}")
    print(report)


if __name__ == "__main__":
    main()
