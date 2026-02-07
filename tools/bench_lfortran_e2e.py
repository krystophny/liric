#!/usr/bin/env python3
"""End-to-end benchmark: lfortran + liric JIT vs lfortran + LLVM native.

For tests that pass both backends, times:
  - liric path:  lfortran --show-llvm → liric_probe_runner (JIT)
  - LLVM path:   lfortran (compile+link+run natively)

Uses only tests with differential_match=True from a prior mass run.
Runs in parallel across all cores.

Usage:
    python3 -m tools.bench_lfortran_e2e [--workers N] [--iters N]
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import statistics
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path


def load_matched_tests(results_path: Path) -> list[dict]:
    tests = []
    with open(results_path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("classification") != "pass":
                continue
            if not r.get("differential_match"):
                continue
            if not r.get("emit_cmd") or not r.get("jit_cmd") or not r.get("diff_ref_cmd"):
                continue
            tests.append(r)
    return tests


def time_cmd(cmd: list[str], timeout: int = 60) -> tuple[float, int]:
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        elapsed = time.monotonic() - t0
        return elapsed, p.returncode
    except subprocess.TimeoutExpired:
        return timeout, -1
    except Exception:
        return 0.0, -1


def run_one(args: tuple) -> dict | None:
    test, iters = args
    emit_cmd = shlex.split(test["emit_cmd"])
    jit_cmd = shlex.split(test["jit_cmd"])
    ref_cmd = shlex.split(test["diff_ref_cmd"])
    case_id = test.get("case_id", "")
    ll_path = None
    for tok in emit_cmd:
        if tok.endswith(".ll"):
            ll_path = tok
            break

    liric_times = []
    llvm_times = []

    for _ in range(iters):
        # liric path: emit .ll + JIT run
        t_emit, rc_emit = time_cmd(emit_cmd)
        if rc_emit != 0:
            return None
        t_jit, rc_jit = time_cmd(jit_cmd)
        if rc_jit < 0:
            return None
        liric_times.append(t_emit + t_jit)

        # LLVM path: compile+link+run
        t_ref, rc_ref = time_cmd(ref_cmd)
        if rc_ref < 0:
            return None
        llvm_times.append(t_ref)

    liric_ms = 1000 * statistics.mean(liric_times)
    llvm_ms = 1000 * statistics.mean(llvm_times)
    emit_ms = 1000 * statistics.mean([t_emit])  # last emit time as proxy
    jit_ms = 1000 * statistics.mean([t_jit])

    return {
        "case_id": case_id,
        "ll_path": ll_path or "",
        "liric_ms": liric_ms,
        "llvm_ms": llvm_ms,
        "emit_ms": emit_ms,
        "jit_ms": jit_ms,
        "speedup": llvm_ms / liric_ms if liric_ms > 0 else 0,
    }


def pct(data: list[float], p: float) -> float:
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def main():
    parser = argparse.ArgumentParser(description="E2E: lfortran+liric vs lfortran+LLVM")
    parser.add_argument("--results", default="/tmp/liric_lfortran_mass/results.jsonl")
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--workers", type=int, default=os.cpu_count())
    args = parser.parse_args()

    results_path = Path(args.results)
    if not results_path.exists():
        print(f"ERROR: {results_path} not found. Run mass tests first.", file=sys.stderr)
        sys.exit(1)

    tests = load_matched_tests(results_path)
    print(f"Found {len(tests)} tests passing both backends, "
          f"running with {args.workers} workers, {args.iters} iters")

    work = [(t, args.iters) for t in tests]
    results = []
    done = 0

    jsonl_path = "/tmp/liric_lfortran_e2e.jsonl"
    with open(jsonl_path, "w") as jf, ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(run_one, w): w for w in work}
        for future in as_completed(futures):
            row = future.result()
            if row:
                results.append(row)
                jf.write(json.dumps(row) + "\n")
            done += 1
            if done % 50 == 0:
                print(f"  {done}/{len(tests)}: {len(results)} matched")

    print(f"\nDone: {len(results)} / {len(tests)} matched")

    if not results:
        print("ERROR: no results", file=sys.stderr)
        sys.exit(1)

    liric_ms = [r["liric_ms"] for r in results]
    llvm_ms = [r["llvm_ms"] for r in results]
    speedups = [r["speedup"] for r in results]
    emit_ms = [r["emit_ms"] for r in results]
    jit_ms = [r["jit_ms"] for r in results]

    faster = sum(1 for s in speedups if s > 1)

    print(f"\n{'='*64}")
    print(f"  lfortran + liric JIT  vs  lfortran + LLVM native")
    print(f"  {len(results)} tests passing both backends, {args.iters} iters each")
    print(f"{'='*64}")
    print(f"\n  {'':12s} {'liric':>12s} {'LLVM native':>12s} {'Speedup':>10s}")
    print(f"  {'':12s} {'─'*12:s} {'─'*12:s} {'─'*10:s}")
    for label, pfn in [("Median", statistics.median), ("Mean", statistics.mean),
                        ("P90", lambda d: pct(d, 90)), ("P95", lambda d: pct(d, 95))]:
        print(f"  {label:12s} {pfn(liric_ms):10.1f} ms {pfn(llvm_ms):10.1f} ms {pfn(speedups):8.1f}x")
    print(f"  {'Aggregate':12s} {sum(liric_ms):10.0f} ms {sum(llvm_ms):10.0f} ms "
          f"{sum(llvm_ms)/sum(liric_ms):8.1f}x")

    print(f"\n  Faster: {faster}/{len(results)} ({100*faster/len(results):.1f}%)")

    print(f"\n  liric breakdown (median):")
    print(f"    emit (lfortran --show-llvm): {statistics.median(emit_ms):.1f} ms")
    print(f"    JIT  (liric_probe_runner):   {statistics.median(jit_ms):.1f} ms")

    print(f"\n  Results: {jsonl_path}")


if __name__ == "__main__":
    main()
