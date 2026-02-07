#!/usr/bin/env python3
"""In-process head-to-head benchmark: liric vs LLVM ORC JIT.

Runs bench_parse_vs_jit (liric) and bench_llvm_jit (LLVM) on every passing
LFortran test, measuring actual compile time without process startup overhead.

Usage:
    python3 -m tools.bench_inprocess_h2h [--results PATH] [--iters N] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import statistics
import subprocess
import sys
from pathlib import Path


def load_passing_ll_files(results_path: Path) -> list[dict]:
    tests = []
    with open(results_path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("classification") != "pass" or not r.get("jit_cmd"):
                continue
            tokens = shlex.split(r["jit_cmd"])
            ll_path = None
            load_libs = []
            i = 0
            while i < len(tokens):
                if tokens[i] == "--load-lib" and i + 1 < len(tokens):
                    load_libs.append(tokens[i + 1])
                    i += 2
                elif tokens[i].endswith(".ll"):
                    ll_path = tokens[i]
                    i += 1
                else:
                    i += 1
            if ll_path and Path(ll_path).exists():
                tests.append({
                    "ll_path": ll_path,
                    "load_libs": load_libs,
                    "case_id": r.get("case_id", ""),
                })
    return tests


def percentile(data: list[float], p: float) -> float:
    if not data:
        return 0.0
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def run_bench(cmd: list[str], timeout: int = 30) -> dict | None:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if p.returncode == 0 and p.stdout.strip():
            r = json.loads(p.stdout.strip())
            if "error" not in r:
                return r
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError):
        pass
    return None


def main():
    parser = argparse.ArgumentParser(description="In-process head-to-head: liric vs LLVM")
    parser.add_argument("--results", default="/tmp/liric_lfortran_mass/results.jsonl")
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--output", default="/tmp/liric_vs_llvm_inprocess.md")
    parser.add_argument("--jsonl", default="/tmp/liric_vs_llvm_inprocess.jsonl")
    parser.add_argument("--liric-bin", default=None)
    parser.add_argument("--llvm-bin", default=None)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    liric_bin = args.liric_bin or str(repo / "build" / "bench_parse_vs_jit")
    llvm_bin = args.llvm_bin or str(repo / "build" / "bench_llvm_jit")

    for label, path in [("liric", liric_bin), ("llvm", llvm_bin)]:
        if not Path(path).exists():
            print(f"ERROR: {label} benchmark not found: {path}", file=sys.stderr)
            sys.exit(1)

    results_path = Path(args.results)
    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run mass tests first.", file=sys.stderr)
        sys.exit(1)

    tests = load_passing_ll_files(results_path)
    print(f"Found {len(tests)} passing tests")

    pairs = []
    liric_only = 0
    llvm_only = 0
    both_fail = 0
    jsonl_f = open(args.jsonl, "w")

    for i, test in enumerate(tests):
        ll = test["ll_path"]
        ll_size = os.path.getsize(ll)

        liric_cmd = [liric_bin, "--json", "--iters", str(args.iters)]
        for lib in test["load_libs"]:
            liric_cmd += ["--load-lib", lib]
        liric_cmd.append(ll)

        llvm_cmd = [llvm_bin, "--json", "--iters", str(args.iters), ll]

        lr = run_bench(liric_cmd)
        llr = run_bench(llvm_cmd)

        row = {"file": ll, "ll_bytes": ll_size, "case_id": test["case_id"]}

        if lr and llr:
            row["liric_parse_ms"] = lr["parse_ms"]
            row["liric_jit_ms"] = lr["jit_ms"]
            row["liric_total_ms"] = lr["total_ms"]
            row["llvm_parse_ms"] = llr["parse_ms"]
            row["llvm_jit_ms"] = llr["jit_ms"]
            row["llvm_total_ms"] = llr["total_ms"]
            row["speedup"] = llr["total_ms"] / lr["total_ms"] if lr["total_ms"] > 0 else 0
            row["builder_speedup"] = llr["total_ms"] / lr["jit_ms"] if lr["jit_ms"] > 0 else 0
            row["matched"] = True
            pairs.append(row)
        elif lr and not llr:
            row["matched"] = False
            row["note"] = "liric_only"
            liric_only += 1
        elif llr and not lr:
            row["matched"] = False
            row["note"] = "llvm_only"
            llvm_only += 1
        else:
            row["matched"] = False
            row["note"] = "both_fail"
            both_fail += 1

        jsonl_f.write(json.dumps(row) + "\n")

        if (i + 1) % 100 == 0:
            print(f"  {i+1}/{len(tests)}: {len(pairs)} matched, "
                  f"{liric_only} liric-only, {llvm_only} llvm-only, {both_fail} both-fail")

    jsonl_f.close()
    print(f"\nDone: {len(pairs)} matched pairs, {liric_only} liric-only, "
          f"{llvm_only} llvm-only, {both_fail} both-fail")

    if not pairs:
        print("ERROR: no matched pairs", file=sys.stderr)
        sys.exit(1)

    liric_totals = [p["liric_total_ms"] for p in pairs]
    llvm_totals = [p["llvm_total_ms"] for p in pairs]
    speedups = [p["speedup"] for p in pairs]
    builder_speedups = [p["builder_speedup"] for p in pairs]
    liric_parse = [p["liric_parse_ms"] for p in pairs]
    liric_jit = [p["liric_jit_ms"] for p in pairs]
    llvm_parse = [p["llvm_parse_ms"] for p in pairs]
    llvm_jit = [p["llvm_jit_ms"] for p in pairs]
    sizes = [p["ll_bytes"] for p in pairs]

    lines = []
    lines.append("# In-Process Head-to-Head: liric vs LLVM ORC JIT")
    lines.append("")
    lines.append(f"- **Matched pairs:** {len(pairs)} / {len(tests)} tests")
    lines.append(f"- **Iterations per test:** {args.iters}")
    lines.append(f"- **liric-only success:** {liric_only}")
    lines.append(f"- **LLVM-only success:** {llvm_only}")
    lines.append(f"- **Both fail:** {both_fail}")
    lines.append("")

    agg_liric = sum(liric_totals)
    agg_llvm = sum(llvm_totals)
    agg_speedup = agg_llvm / agg_liric if agg_liric > 0 else 0
    agg_builder = agg_llvm / sum(liric_jit) if sum(liric_jit) > 0 else 0
    lines.append("## Aggregate Totals")
    lines.append("")
    lines.append(f"- **liric total:** {agg_liric:.1f} ms")
    lines.append(f"- **LLVM total:** {agg_llvm:.1f} ms")
    lines.append(f"- **Speedup (text path):** {agg_speedup:.1f}x")
    lines.append(f"- **Speedup (builder API):** {agg_builder:.1f}x")
    lines.append("")

    lines.append("## Per-Test Compile Time (milliseconds)")
    lines.append("")
    lines.append("| Metric | liric | LLVM ORC | Speedup | Builder Speedup |")
    lines.append("|--------|------:|---------:|--------:|----------------:|")
    for label, pfn in [
        ("Median", statistics.median),
        ("Mean", statistics.mean),
        ("P90", lambda d: percentile(d, 90)),
        ("P95", lambda d: percentile(d, 95)),
        ("Min", min),
        ("Max", max),
    ]:
        lines.append(
            f"| {label} | {pfn(liric_totals):.3f} | {pfn(llvm_totals):.3f} | "
            f"{pfn(speedups):.1f}x | {pfn(builder_speedups):.1f}x |"
        )
    lines.append("")

    lines.append("## Phase Breakdown (milliseconds)")
    lines.append("")
    lines.append("| Metric | liric parse | liric JIT | LLVM parse | LLVM JIT |")
    lines.append("|--------|------------:|----------:|-----------:|---------:|")
    for label, pfn in [
        ("Median", statistics.median),
        ("Mean", statistics.mean),
        ("P90", lambda d: percentile(d, 90)),
        ("P95", lambda d: percentile(d, 95)),
    ]:
        lines.append(
            f"| {label} | {pfn(liric_parse):.3f} | {pfn(liric_jit):.3f} | "
            f"{pfn(llvm_parse):.3f} | {pfn(llvm_jit):.3f} |"
        )
    lines.append("")

    lines.append("## Speedup Distribution")
    lines.append("")
    thresholds = [50, 20, 10, 5, 2, 1]
    for t in thresholds:
        count = sum(1 for s in speedups if s >= t)
        pct = 100.0 * count / len(speedups)
        lines.append(f"- >={t}x faster: {count} ({pct:.1f}%)")
    slower = sum(1 for s in speedups if s < 1)
    lines.append(f"- liric slower: {slower} ({100.0*slower/len(speedups):.1f}%)")
    lines.append("")

    lines.append("## .ll File Size Distribution")
    lines.append("")
    lines.append(f"- Min: {min(sizes):,} bytes")
    lines.append(f"- Median: {statistics.median(sizes):,.0f} bytes")
    lines.append(f"- Mean: {statistics.mean(sizes):,.0f} bytes")
    lines.append(f"- Max: {max(sizes):,} bytes")
    lines.append("")

    report = "\n".join(lines)
    Path(args.output).write_text(report)
    print(f"\nReport: {args.output}")
    print(f"JSONL: {args.jsonl}")
    print(report)


if __name__ == "__main__":
    main()
