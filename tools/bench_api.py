#!/usr/bin/env python3
"""API benchmark: liric JIT vs lfortran LLVM native, -O0.

Reads /tmp/liric_bench/compat_api.txt (from bench_compat_check) and times the
full pipeline for each path:
  - LLVM: lfortran compile + run
  - liric: lfortran --show-llvm + liric_probe_runner --timing JIT

Reports both wall-clock and JIT-internal timing (parse + compile only,
excluding process startup and dlopen overhead).

Usage:
    python3 -m tools.bench_api [--iters N]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path

BENCH_DIR = Path("/tmp/liric_bench")
LL_DIR = BENCH_DIR / "ll"
BIN_DIR = BENCH_DIR / "bin"

TIMING_RE = re.compile(
    r"TIMING\s+read_us=([\d.]+)\s+parse_us=([\d.]+)\s+jit_create_us=([\d.]+)\s+"
    r"load_lib_us=([\d.]+)\s+compile_us=([\d.]+)\s+total_us=([\d.]+)"
)


def detect_paths(args: argparse.Namespace) -> dict:
    repo = Path(__file__).resolve().parent.parent
    meta = repo.parent

    lfortran = args.lfortran or str(meta / "lfortran" / "build" / "src" / "bin" / "lfortran")
    probe = args.probe_runner or str(repo / "build" / "liric_probe_runner")
    runtime = args.runtime_lib or str(meta / "lfortran" / "build" / "src" / "runtime" / "liblfortran_runtime.dylib")
    if not Path(runtime).exists():
        runtime = str(meta / "lfortran" / "build" / "src" / "runtime" / "liblfortran_runtime.so")
    integration_dir = args.integration_dir or str(meta / "lfortran" / "integration_tests")

    paths = {
        "lfortran": lfortran,
        "probe_runner": probe,
        "runtime_lib": runtime,
        "integration_dir": integration_dir,
    }

    for label, path in paths.items():
        if not Path(path).exists():
            print(f"ERROR: {label} not found: {path}", file=sys.stderr)
            sys.exit(1)

    return paths


def load_test_list(compat_path: Path, options_path: Path) -> list[dict]:
    options_by_name = {}
    if options_path.exists():
        with open(options_path) as f:
            for line in f:
                rec = json.loads(line)
                options_by_name[rec["name"]] = rec.get("options", "")

    tests = []
    with open(compat_path) as f:
        for line in f:
            name = line.strip()
            if name:
                tests.append({"name": name, "options": options_by_name.get(name, "")})
    return tests


def time_cmd(cmd: list[str], timeout: int) -> tuple[float, int, str]:
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        elapsed = time.monotonic() - t0
        stderr = p.stderr.decode("utf-8", errors="replace")
        return elapsed, p.returncode, stderr
    except subprocess.TimeoutExpired:
        return timeout, -99, "timeout"
    except Exception as exc:
        return 0.0, -1, str(exc)


def time_emit_ll(cmd: list[str], out_path: str, timeout: int) -> tuple[float, int]:
    """Time lfortran --show-llvm, redirecting stdout to a file."""
    t0 = time.monotonic()
    try:
        with open(out_path, "w") as f:
            p = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE, timeout=timeout)
        elapsed = time.monotonic() - t0
        return elapsed, p.returncode
    except subprocess.TimeoutExpired:
        return timeout, -99
    except Exception:
        return 0.0, -1


def run_liric_timed(cmd: list[str], timeout: int) -> tuple[float, int, dict | None]:
    """Run liric_probe_runner with --timing, return (wall_s, rc, timing_dict)."""
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        wall_s = time.monotonic() - t0
        stderr = p.stderr.decode("utf-8", errors="replace")
        timing = None
        m = TIMING_RE.search(stderr)
        if m:
            timing = {
                "read_us": float(m.group(1)),
                "parse_us": float(m.group(2)),
                "jit_create_us": float(m.group(3)),
                "load_lib_us": float(m.group(4)),
                "compile_us": float(m.group(5)),
                "total_us": float(m.group(6)),
            }
        return wall_s, p.returncode, timing
    except subprocess.TimeoutExpired:
        return timeout, -99, None
    except Exception:
        return 0.0, -1, None


def pct(data: list[float], p: float) -> float:
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def main() -> None:
    parser = argparse.ArgumentParser(
        description="API benchmark: liric JIT vs lfortran LLVM native"
    )
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--lfortran", default=None)
    parser.add_argument("--probe-runner", default=None)
    parser.add_argument("--runtime-lib", default=None)
    parser.add_argument("--integration-dir", default=None)
    args = parser.parse_args()

    paths = detect_paths(args)

    compat_path = BENCH_DIR / "compat_api.txt"
    options_path = BENCH_DIR / "compat_api_options.jsonl"
    if not compat_path.exists():
        print("ERROR: compat_api.txt not found. Run bench_compat_check first.", file=sys.stderr)
        sys.exit(1)

    tests = load_test_list(compat_path, options_path)
    if not tests:
        print("ERROR: no compatible tests found in compat_api.txt", file=sys.stderr)
        sys.exit(1)

    print(f"Benchmarking {len(tests)} tests, {args.iters} iterations each")

    lfortran = paths["lfortran"]
    probe = paths["probe_runner"]
    runtime = paths["runtime_lib"]
    integration_dir = paths["integration_dir"]
    timeout = args.timeout

    LL_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(parents=True, exist_ok=True)

    jsonl_path = BENCH_DIR / "bench_api.jsonl"
    results = []

    with open(jsonl_path, "w") as jf:
        for idx, test in enumerate(tests):
            name = test["name"]
            opts = shlex.split(test["options"]) if test["options"] else []
            source = str(Path(integration_dir) / f"{name}.f90")
            ll_path = str(LL_DIR / f"{name}.ll")
            bin_path = str(BIN_DIR / name)

            if not Path(source).exists():
                print(f"  [{idx+1}/{len(tests)}] {name}: source not found, skipping")
                continue

            llvm_times = []
            liric_wall_times = []
            liric_jit_times = []
            skipped = False

            for it in range(args.iters):
                t_compile, rc, _ = time_cmd(
                    [lfortran, "--no-color"] + opts + [source, "-o", bin_path],
                    timeout,
                )
                if rc != 0:
                    skipped = True
                    break
                t_run, rc, _ = time_cmd([bin_path], timeout)
                if rc < 0:
                    skipped = True
                    break
                llvm_times.append(t_compile + t_run)

                t_emit, rc = time_emit_ll(
                    [lfortran, "--no-color", "--show-llvm"] + opts + [source],
                    ll_path,
                    timeout,
                )
                if rc != 0:
                    skipped = True
                    break
                t_jit_wall, rc, timing = run_liric_timed(
                    [probe, "--timing", "--sig", "i32_argc_argv", "--load-lib", runtime, ll_path],
                    timeout,
                )
                if rc < 0:
                    skipped = True
                    break
                liric_wall_times.append(t_emit + t_jit_wall)
                if timing:
                    jit_ms = (timing["parse_us"] + timing["compile_us"]) / 1000.0
                    liric_jit_times.append(jit_ms)

            if skipped or not llvm_times or not liric_wall_times:
                print(f"  [{idx+1}/{len(tests)}] {name}: skipped (runtime error)")
                continue

            llvm_ms = [t * 1000 for t in llvm_times]
            liric_wall_ms = [t * 1000 for t in liric_wall_times]
            wall_speedup = statistics.median(llvm_ms) / statistics.median(liric_wall_ms) if statistics.median(liric_wall_ms) > 0 else 0

            row = {
                "name": name,
                "llvm_median_ms": round(statistics.median(llvm_ms), 3),
                "liric_wall_median_ms": round(statistics.median(liric_wall_ms), 3),
                "wall_speedup": round(wall_speedup, 3),
                "iters": args.iters,
            }

            if liric_jit_times:
                liric_jit_median = statistics.median(liric_jit_times)
                jit_speedup = statistics.median(llvm_ms) / liric_jit_median if liric_jit_median > 0 else 0
                row["liric_jit_median_ms"] = round(liric_jit_median, 4)
                row["jit_speedup"] = round(jit_speedup, 1)

            results.append(row)
            jf.write(json.dumps(row) + "\n")

            jit_tag = ""
            if "jit_speedup" in row:
                jit_tag = f" jit={row['liric_jit_median_ms']:.3f}ms({row['jit_speedup']:.0f}x)"
            marker = "+" if wall_speedup > 1 else "-"
            print(f"  [{idx+1}/{len(tests)}] {name}: "
                  f"llvm={statistics.median(llvm_ms):.1f}ms "
                  f"liric={statistics.median(liric_wall_ms):.1f}ms "
                  f"{marker}{wall_speedup:.2f}x{jit_tag}")

    if not results:
        print("ERROR: no benchmark results", file=sys.stderr)
        sys.exit(1)

    llvm_med = [r["llvm_median_ms"] for r in results]
    liric_wall_med = [r["liric_wall_median_ms"] for r in results]
    wall_speedups = [r["wall_speedup"] for r in results]
    wall_faster = sum(1 for s in wall_speedups if s > 1)

    has_jit = sum(1 for r in results if "jit_speedup" in r) > len(results) // 2
    liric_jit_med = [r["liric_jit_median_ms"] for r in results if "liric_jit_median_ms" in r]
    jit_speedups = [r["jit_speedup"] for r in results if "jit_speedup" in r]

    print(f"\n{'='*72}")
    print(f"  liric JIT  vs  lfortran LLVM native  (API path, -O0)")
    print(f"  {len(results)} tests, {args.iters} iterations each")
    print(f"{'='*72}")

    print(f"\n  WALL-CLOCK (full subprocess pipeline)")
    print(f"  {'':12s} {'liric':>12s} {'LLVM native':>12s} {'Speedup':>10s}")
    print(f"  {'':12s} {'---'*4:>12s} {'---'*4:>12s} {'---'*3:>10s}")
    for label, pfn in [
        ("Median", statistics.median),
        ("Mean", statistics.mean),
        ("P90", lambda d: pct(d, 90)),
        ("P95", lambda d: pct(d, 95)),
    ]:
        print(f"  {label:12s} {pfn(liric_wall_med):10.1f} ms {pfn(llvm_med):10.1f} ms {pfn(wall_speedups):8.2f}x")
    agg_wall = sum(llvm_med) / sum(liric_wall_med) if sum(liric_wall_med) > 0 else 0
    print(f"  {'Aggregate':12s} {sum(liric_wall_med):10.0f} ms {sum(llvm_med):10.0f} ms {agg_wall:8.2f}x")
    print(f"\n  Faster: {wall_faster}/{len(results)} ({100*wall_faster/len(results):.1f}%)")

    if has_jit and liric_jit_med:
        jit_faster = sum(1 for s in jit_speedups if s > 1)
        print(f"\n  JIT-INTERNAL (parse + compile only, vs LLVM native full pipeline)")
        print(f"  {'':12s} {'liric JIT':>12s} {'LLVM native':>12s} {'Speedup':>10s}")
        print(f"  {'':12s} {'---'*4:>12s} {'---'*4:>12s} {'---'*3:>10s}")
        for label, pfn in [
            ("Median", statistics.median),
            ("Mean", statistics.mean),
            ("P90", lambda d: pct(d, 90)),
            ("P95", lambda d: pct(d, 95)),
        ]:
            print(f"  {label:12s} {pfn(liric_jit_med):10.3f} ms {pfn(llvm_med):10.1f} ms {pfn(jit_speedups):8.1f}x")
        agg_jit = sum(llvm_med) / sum(liric_jit_med) if sum(liric_jit_med) > 0 else 0
        print(f"  {'Aggregate':12s} {sum(liric_jit_med):10.1f} ms {sum(llvm_med):10.0f} ms {agg_jit:8.1f}x")
        print(f"\n  Faster: {jit_faster}/{len(results)} ({100*jit_faster/len(results):.1f}%)")

    print(f"\n  Results: {jsonl_path}")


if __name__ == "__main__":
    main()
