#!/usr/bin/env python3
"""Head-to-head benchmark: liric vs LLVM ORC JIT.

Runs both JIT compilers on every passing LFortran test in parallel.
Only counts files that pass BOTH compilers (matched pairs).
Both compilers load the same runtime library for honest comparison.

Usage:
    python3 -m tools.bench_h2h [--workers N] [--iters N]
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import statistics
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path


def load_passing_tests(results_path: Path) -> list[dict]:
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


def run_one(args: tuple) -> dict | None:
    test, liric_bin, llvm_bin, iters = args
    ll = test["ll_path"]
    ll_size = os.path.getsize(ll)

    liric_cmd = [liric_bin, "--json", "--iters", str(iters)]
    llvm_cmd = [llvm_bin, "--json", "--iters", str(iters)]
    for lib in test["load_libs"]:
        liric_cmd += ["--load-lib", lib]
        llvm_cmd += ["--load-lib", lib]
    liric_cmd.append(ll)
    llvm_cmd.append(ll)

    try:
        r1 = subprocess.run(liric_cmd, capture_output=True, text=True, timeout=30)
        d1 = json.loads(r1.stdout.strip()) if r1.returncode == 0 else None
        if d1 and "error" in d1:
            d1 = None
    except Exception:
        d1 = None

    try:
        r2 = subprocess.run(llvm_cmd, capture_output=True, text=True, timeout=30)
        d2 = json.loads(r2.stdout.strip()) if r2.returncode == 0 else None
        if d2 and "error" in d2:
            d2 = None
    except Exception:
        d2 = None

    status = "matched" if d1 and d2 else "liric_only" if d1 else "llvm_only" if d2 else "both_fail"
    row = {"file": ll, "ll_bytes": ll_size, "case_id": test["case_id"], "status": status}
    if d1 and d2:
        row.update({
            "liric_parse_ms": d1["parse_ms"],
            "liric_jit_ms": d1["jit_ms"],
            "liric_total_ms": d1["total_ms"],
            "llvm_parse_ms": d2["parse_ms"],
            "llvm_jit_ms": d2["jit_ms"],
            "llvm_total_ms": d2["total_ms"],
        })
    return row


def pct(data: list[float], p: float) -> float:
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def main():
    parser = argparse.ArgumentParser(description="H2H: liric vs LLVM ORC JIT")
    parser.add_argument("--results", default="/tmp/liric_lfortran_mass/results.jsonl")
    parser.add_argument("--iters", type=int, default=100)
    parser.add_argument("--workers", type=int, default=os.cpu_count())
    parser.add_argument("--liric-bin", default=None)
    parser.add_argument("--llvm-bin", default=None)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    liric_bin = args.liric_bin or str(repo / "build-rel" / "bench_parse_vs_jit")
    if not Path(liric_bin).exists():
        liric_bin = str(repo / "build" / "bench_parse_vs_jit")
    llvm_bin = args.llvm_bin or str(repo / "build" / "bench_llvm_jit")

    for label, path in [("liric", liric_bin), ("llvm", llvm_bin)]:
        if not Path(path).exists():
            print(f"ERROR: {label} benchmark not found: {path}", file=sys.stderr)
            sys.exit(1)

    results_path = Path(args.results)
    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run mass tests first.", file=sys.stderr)
        sys.exit(1)

    tests = load_passing_tests(results_path)
    print(f"Found {len(tests)} passing tests, running with {args.workers} workers, {args.iters} iters")

    work = [(t, liric_bin, llvm_bin, args.iters) for t in tests]
    results = []
    done = 0

    jsonl_path = "/tmp/liric_h2h.jsonl"
    with open(jsonl_path, "w") as jf, ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(run_one, w): w for w in work}
        for future in as_completed(futures):
            row = future.result()
            if row:
                results.append(row)
                jf.write(json.dumps(row) + "\n")
            done += 1
            if done % 100 == 0:
                matched = sum(1 for r in results if r["status"] == "matched")
                print(f"  {done}/{len(tests)}: {matched} matched pairs")

    pairs = [r for r in results if r["status"] == "matched"]
    liric_only = sum(1 for r in results if r["status"] == "liric_only")
    llvm_only = sum(1 for r in results if r["status"] == "llvm_only")
    both_fail = sum(1 for r in results if r["status"] == "both_fail")

    print(f"\nDone: {len(pairs)} matched, {liric_only} liric-only, "
          f"{llvm_only} llvm-only, {both_fail} both-fail")

    if not pairs:
        print("ERROR: no matched pairs", file=sys.stderr)
        sys.exit(1)

    lt = [p["liric_total_ms"] for p in pairs]
    lj = [p["liric_jit_ms"] for p in pairs]
    lp = [p["liric_parse_ms"] for p in pairs]
    et = [p["llvm_total_ms"] for p in pairs]
    ej = [p["llvm_jit_ms"] for p in pairs]
    ep = [p["llvm_parse_ms"] for p in pairs]
    speedups = [p["llvm_total_ms"] / p["liric_total_ms"] for p in pairs if p["liric_total_ms"] > 0]
    jit_sp = [p["llvm_jit_ms"] / p["liric_jit_ms"] for p in pairs if p["liric_jit_ms"] > 0]

    faster = sum(1 for s in speedups if s > 1)

    print(f"\n{'='*60}")
    print(f"  liric vs LLVM ORC JIT  ({len(pairs)} matched files)")
    print(f"{'='*60}")
    print(f"\n  {'':12s} {'liric':>10s} {'LLVM ORC':>10s} {'Speedup':>10s}")
    print(f"  {'':12s} {'─'*10:s} {'─'*10:s} {'─'*10:s}")
    for label, pfn in [("Median", statistics.median), ("Mean", statistics.mean),
                        ("P90", lambda d: pct(d, 90)), ("P95", lambda d: pct(d, 95))]:
        print(f"  {label:12s} {pfn(lt):8.3f} ms {pfn(et):8.3f} ms {pfn(speedups):8.1f}x")
    print(f"  {'Aggregate':12s} {sum(lt):8.0f} ms {sum(et):8.0f} ms {sum(et)/sum(lt):8.1f}x")
    print(f"\n  Faster: {faster}/{len(pairs)} ({100*faster/len(pairs):.1f}%)")

    print(f"\n  JIT-only:  liric {statistics.median(lj):.3f} ms vs LLVM {statistics.median(ej):.3f} ms"
          f"  ({statistics.median(jit_sp):.1f}x)")
    print(f"  Parse:     liric {statistics.median(lp):.3f} ms vs LLVM {statistics.median(ep):.3f} ms")

    print(f"\n  Results: {jsonl_path}")


if __name__ == "__main__":
    main()
