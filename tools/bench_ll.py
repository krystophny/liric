#!/usr/bin/env python3
"""LL-file benchmark: liric JIT vs lli, -O0.

Reads /tmp/liric_bench/compat_ll.txt (from bench_compat_check) and times
JIT execution of pre-generated .ll files:
  - liric: liric_probe_runner --load-lib <runtime> file.ll
  - lli:   lli -O0 file.ll  (with DYLD_LIBRARY_PATH/LD_LIBRARY_PATH set)

Usage:
    python3 -m tools.bench_ll [--iters N]
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
from pathlib import Path

BENCH_DIR = Path("/tmp/liric_bench")
LL_DIR = BENCH_DIR / "ll"


def detect_paths(args: argparse.Namespace) -> dict:
    repo = Path(__file__).resolve().parent.parent
    meta = repo.parent

    lfortran = args.lfortran or str(meta / "lfortran" / "build" / "src" / "bin" / "lfortran")
    probe = args.probe_runner or str(repo / "build" / "liric_probe_runner")
    runtime = args.runtime_lib or str(meta / "lfortran" / "build" / "src" / "runtime" / "liblfortran_runtime.dylib")
    if not Path(runtime).exists():
        runtime = str(meta / "lfortran" / "build" / "src" / "runtime" / "liblfortran_runtime.so")
    lli = args.lli or "/opt/homebrew/opt/llvm/bin/lli"
    if not Path(lli).exists():
        lli = "lli"
    integration_dir = args.integration_dir or str(meta / "lfortran" / "integration_tests")

    paths = {
        "lfortran": lfortran,
        "probe_runner": probe,
        "runtime_lib": runtime,
        "lli": lli,
        "integration_dir": integration_dir,
    }

    for label, path in paths.items():
        if label == "lli":
            continue
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


def time_cmd(cmd: list[str], timeout: int, env: dict | None = None) -> tuple[float, int]:
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
        elapsed = time.monotonic() - t0
        return elapsed, p.returncode
    except subprocess.TimeoutExpired:
        return timeout, -99
    except Exception:
        return 0.0, -1


def pct(data: list[float], p: float) -> float:
    s = sorted(data)
    k = (len(s) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (k - f) * (s[c] - s[f])


def main() -> None:
    parser = argparse.ArgumentParser(
        description="LL-file benchmark: liric JIT vs lli"
    )
    parser.add_argument("--iters", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--lfortran", default=None)
    parser.add_argument("--probe-runner", default=None)
    parser.add_argument("--runtime-lib", default=None)
    parser.add_argument("--lli", default=None)
    parser.add_argument("--integration-dir", default=None)
    args = parser.parse_args()

    paths = detect_paths(args)

    compat_path = BENCH_DIR / "compat_ll.txt"
    options_path = BENCH_DIR / "compat_ll_options.jsonl"
    if not compat_path.exists():
        print("ERROR: compat_ll.txt not found. Run bench_compat_check first.", file=sys.stderr)
        sys.exit(1)

    tests = load_test_list(compat_path, options_path)
    if not tests:
        print("ERROR: no compatible tests found in compat_ll.txt", file=sys.stderr)
        sys.exit(1)

    lfortran = paths["lfortran"]
    probe = paths["probe_runner"]
    runtime = paths["runtime_lib"]
    lli = paths["lli"]
    integration_dir = paths["integration_dir"]
    timeout = args.timeout

    LL_DIR.mkdir(parents=True, exist_ok=True)

    # Pre-generate all .ll files
    print(f"Pre-generating {len(tests)} .ll files ...")
    valid_tests = []
    for test in tests:
        name = test["name"]
        opts = shlex.split(test["options"]) if test["options"] else []
        source = str(Path(integration_dir) / f"{name}.f90")
        ll_path = str(LL_DIR / f"{name}.ll")

        if not Path(source).exists():
            continue

        cmd = [lfortran, "--no-color", "--show-llvm"] + opts + [source]
        try:
            with open(ll_path, "w") as f:
                p = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE, timeout=timeout)
            if p.returncode == 0 and Path(ll_path).stat().st_size > 0:
                valid_tests.append(test)
        except Exception:
            pass

    print(f"  {len(valid_tests)}/{len(tests)} .ll files generated")

    if not valid_tests:
        print("ERROR: no .ll files generated", file=sys.stderr)
        sys.exit(1)

    runtime_dir = str(Path(runtime).parent)
    lli_env = os.environ.copy()
    lli_env["DYLD_LIBRARY_PATH"] = runtime_dir
    lli_env["LD_LIBRARY_PATH"] = runtime_dir

    print(f"Benchmarking {len(valid_tests)} tests, {args.iters} iterations each")

    jsonl_path = BENCH_DIR / "bench_ll.jsonl"
    results = []

    with open(jsonl_path, "w") as jf:
        for idx, test in enumerate(valid_tests):
            name = test["name"]
            ll_path = str(LL_DIR / f"{name}.ll")

            liric_times = []
            lli_times = []
            skipped = False

            for it in range(args.iters):
                # liric path
                t_liric, rc = time_cmd(
                    [probe, "--load-lib", runtime, ll_path],
                    timeout,
                )
                if rc < 0:
                    skipped = True
                    break
                liric_times.append(t_liric)

                # lli path
                t_lli, rc = time_cmd(
                    [lli, "-O0", ll_path],
                    timeout,
                    env=lli_env,
                )
                if rc < 0:
                    skipped = True
                    break
                lli_times.append(t_lli)

            if skipped or not liric_times or not lli_times:
                print(f"  [{idx+1}/{len(valid_tests)}] {name}: skipped (runtime error)")
                continue

            liric_ms = [t * 1000 for t in liric_times]
            lli_ms = [t * 1000 for t in lli_times]
            speedup = statistics.median(lli_ms) / statistics.median(liric_ms) if statistics.median(liric_ms) > 0 else 0

            row = {
                "name": name,
                "liric_median_ms": round(statistics.median(liric_ms), 3),
                "lli_median_ms": round(statistics.median(lli_ms), 3),
                "liric_mean_ms": round(statistics.mean(liric_ms), 3),
                "lli_mean_ms": round(statistics.mean(lli_ms), 3),
                "speedup": round(speedup, 3),
                "iters": args.iters,
            }
            results.append(row)
            jf.write(json.dumps(row) + "\n")

            marker = "+" if speedup > 1 else "-"
            print(f"  [{idx+1}/{len(valid_tests)}] {name}: "
                  f"liric={statistics.median(liric_ms):.1f}ms "
                  f"lli={statistics.median(lli_ms):.1f}ms "
                  f"{marker}{speedup:.2f}x")

    if not results:
        print("ERROR: no benchmark results", file=sys.stderr)
        sys.exit(1)

    liric_med = [r["liric_median_ms"] for r in results]
    lli_med = [r["lli_median_ms"] for r in results]
    speedups = [r["speedup"] for r in results]
    faster = sum(1 for s in speedups if s > 1)

    print(f"\n{'='*64}")
    print(f"  liric JIT  vs  lli  (LL-file path, -O0)")
    print(f"  {len(results)} tests, {args.iters} iterations each")
    print(f"{'='*64}")
    print(f"\n  {'':12s} {'liric':>12s} {'lli':>12s} {'Speedup':>10s}")
    print(f"  {'':12s} {'---'*4:>12s} {'---'*4:>12s} {'---'*3:>10s}")

    for label, pfn in [
        ("Median", statistics.median),
        ("Mean", statistics.mean),
        ("P90", lambda d: pct(d, 90)),
        ("P95", lambda d: pct(d, 95)),
    ]:
        print(f"  {label:12s} {pfn(liric_med):10.1f} ms {pfn(lli_med):10.1f} ms {pfn(speedups):8.2f}x")

    agg_speedup = sum(lli_med) / sum(liric_med) if sum(liric_med) > 0 else 0
    print(f"  {'Aggregate':12s} {sum(liric_med):10.0f} ms {sum(lli_med):10.0f} ms {agg_speedup:8.2f}x")

    print(f"\n  Faster: {faster}/{len(results)} ({100*faster/len(results):.1f}%)")
    print(f"\n  Results: {jsonl_path}")


if __name__ == "__main__":
    main()
