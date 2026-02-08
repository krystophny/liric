#!/usr/bin/env python3
"""Correctness check: compare liric JIT and lli output against lfortran LLVM native.

Parses lfortran integration_tests/CMakeLists.txt, runs each eligible test three
ways, and produces compatibility lists for the benchmark scripts.

Usage:
    python3 -m tools.bench_compat_check [--workers N] [--timeout N]
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from tools.lfortran_mass.integration_tests_cmake import (
    iter_integration_entries,
)

SKIP_LABELS = {"llvm_omp", "llvm2", "llvm_rtlib"}

BENCH_DIR = Path("/tmp/liric_bench")
LL_DIR = BENCH_DIR / "ll"
BIN_DIR = BENCH_DIR / "bin"


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
    cmake_path = args.cmake or str(meta / "lfortran" / "integration_tests" / "CMakeLists.txt")

    paths = {
        "lfortran": lfortran,
        "probe_runner": probe,
        "runtime_lib": runtime,
        "lli": lli,
        "cmake_path": cmake_path,
    }

    for label, path in paths.items():
        if label == "lli":
            continue
        if not Path(path).exists():
            print(f"ERROR: {label} not found: {path}", file=sys.stderr)
            sys.exit(1)

    return paths


def collect_tests(cmake_path: str) -> list[dict]:
    tests = []
    for entry in iter_integration_entries(Path(cmake_path)):
        if not entry.llvm:
            continue
        if "llvm" not in entry.labels:
            continue
        if entry.expected_fail:
            continue
        if entry.extrafiles:
            continue
        if SKIP_LABELS & set(entry.labels):
            continue
        if not entry.source_path.exists():
            continue

        tests.append({
            "name": entry.name,
            "source": str(entry.source_path),
            "options": entry.options,
        })
    return tests


def run_cmd(cmd: list[str], timeout: int, env: dict | None = None) -> tuple[int, str, str]:
    try:
        p = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout,
            env=env,
        )
        stdout = p.stdout.decode("utf-8", errors="replace")
        stderr = p.stderr.decode("utf-8", errors="replace")
        return p.returncode, stdout, stderr
    except subprocess.TimeoutExpired:
        return -99, "", "timeout"
    except Exception as exc:
        return -1, "", str(exc)


def emit_ll(cmd: list[str], out_path: str, timeout: int) -> tuple[int, str]:
    """Run lfortran --show-llvm and redirect stdout to a file."""
    try:
        with open(out_path, "w") as f:
            p = subprocess.run(
                cmd,
                stdout=f,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout,
            )
        return p.returncode, p.stderr
    except subprocess.TimeoutExpired:
        return -99, "timeout"
    except Exception as exc:
        return -1, str(exc)


def normalize_output(text: str) -> str:
    lines = text.rstrip("\n").split("\n")
    return "\n".join(line.rstrip() for line in lines)


def check_one_test(args: tuple) -> dict:
    test, paths, timeout = args
    name = test["name"]
    source = test["source"]
    opts = shlex.split(test["options"]) if test["options"] else []

    lfortran = paths["lfortran"]
    probe = paths["probe_runner"]
    runtime = paths["runtime_lib"]
    lli = paths["lli"]

    ll_path = str(LL_DIR / f"{name}.ll")
    bin_path = str(BIN_DIR / name)

    result = {
        "name": name,
        "source": source,
        "options": test["options"],
        "llvm_ok": False,
        "liric_ok": False,
        "lli_ok": False,
        "liric_match": False,
        "lli_match": False,
        "error": "",
    }

    # Step 1: emit .ll (--show-llvm outputs to stdout, redirect to file)
    emit_cmd = [lfortran, "--no-color", "--show-llvm"] + opts + [source]
    rc, stderr = emit_ll(emit_cmd, ll_path, timeout)
    if rc != 0:
        result["error"] = f"emit failed (rc={rc}): {stderr[:200]}"
        return result

    # Step 2: LLVM native compile + run
    compile_cmd = [lfortran, "--no-color"] + opts + [source, "-o", bin_path]
    rc, _, stderr = run_cmd(compile_cmd, timeout)
    if rc != 0:
        result["error"] = f"compile failed (rc={rc}): {stderr[:200]}"
        return result

    rc, llvm_stdout, stderr = run_cmd([bin_path], timeout)
    if rc < 0:
        result["error"] = f"llvm run failed (rc={rc}): {stderr[:200]}"
        return result
    result["llvm_ok"] = True
    llvm_out = normalize_output(llvm_stdout)
    result["llvm_rc"] = rc

    # Step 3: liric JIT
    jit_cmd = [probe, "--load-lib", runtime, ll_path]
    jit_rc, liric_stdout, stderr = run_cmd(jit_cmd, timeout)
    if jit_rc < 0:
        result["error"] = f"liric jit failed (rc={jit_rc}): {stderr[:200]}"
    else:
        result["liric_ok"] = True
        liric_out = normalize_output(liric_stdout)
        result["liric_match"] = (liric_out == llvm_out and jit_rc == rc)

    # Step 4: lli
    runtime_dir = str(Path(runtime).parent)
    lli_env = os.environ.copy()
    lli_env["DYLD_LIBRARY_PATH"] = runtime_dir
    lli_env["LD_LIBRARY_PATH"] = runtime_dir
    lli_cmd = [lli, "-O0", ll_path]
    lli_rc, lli_stdout, stderr = run_cmd(lli_cmd, timeout, env=lli_env)
    if lli_rc < 0:
        pass
    else:
        result["lli_ok"] = True
        lli_out = normalize_output(lli_stdout)
        result["lli_match"] = (lli_out == llvm_out and lli_rc == rc)

    return result


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compatibility check: liric/lli vs lfortran LLVM native"
    )
    parser.add_argument("--workers", type=int, default=os.cpu_count())
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--limit", type=int, default=0,
                        help="Limit number of tests (0 = all)")
    parser.add_argument("--lfortran", default=None)
    parser.add_argument("--probe-runner", default=None)
    parser.add_argument("--runtime-lib", default=None)
    parser.add_argument("--lli", default=None)
    parser.add_argument("--cmake", default=None)
    args = parser.parse_args()

    paths = detect_paths(args)

    LL_DIR.mkdir(parents=True, exist_ok=True)
    BIN_DIR.mkdir(parents=True, exist_ok=True)

    tests = collect_tests(paths["cmake_path"])
    if args.limit > 0:
        tests = tests[:args.limit]
    print(f"Found {len(tests)} eligible integration tests")
    print(f"Workers: {args.workers}, timeout: {args.timeout}s", flush=True)

    t0 = time.monotonic()
    results = []
    done = 0

    jsonl_path = BENCH_DIR / "compat_check.jsonl"
    work = [(t, paths, args.timeout) for t in tests]

    with open(jsonl_path, "w") as jf, ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(check_one_test, w): w for w in work}
        for future in as_completed(futures):
            row = future.result()
            results.append(row)
            jf.write(json.dumps(row) + "\n")
            jf.flush()
            done += 1
            if done % 50 == 0 or done == len(tests):
                liric_match = sum(1 for r in results if r["liric_match"])
                lli_match = sum(1 for r in results if r["lli_match"])
                print(f"  {done}/{len(tests)}: liric={liric_match} lli={lli_match}",
                      flush=True)

    elapsed = time.monotonic() - t0

    compat_api = sorted(r["name"] for r in results if r["liric_match"])
    compat_ll = sorted(r["name"] for r in results if r["liric_match"] and r["lli_match"])

    api_path = BENCH_DIR / "compat_api.txt"
    ll_path = BENCH_DIR / "compat_ll.txt"

    with open(api_path, "w") as f:
        for name in compat_api:
            f.write(name + "\n")

    with open(ll_path, "w") as f:
        for name in compat_ll:
            f.write(name + "\n")

    options_by_name = {t["name"]: t["options"] for t in tests}
    options_api_path = BENCH_DIR / "compat_api_options.jsonl"
    options_ll_path = BENCH_DIR / "compat_ll_options.jsonl"

    with open(options_api_path, "w") as f:
        for name in compat_api:
            f.write(json.dumps({"name": name, "options": options_by_name.get(name, "")}) + "\n")

    with open(options_ll_path, "w") as f:
        for name in compat_ll:
            f.write(json.dumps({"name": name, "options": options_by_name.get(name, "")}) + "\n")

    llvm_ok = sum(1 for r in results if r["llvm_ok"])
    liric_ok = sum(1 for r in results if r["liric_ok"])
    lli_ok = sum(1 for r in results if r["lli_ok"])
    liric_match = len(compat_api)
    lli_match = len(compat_ll)

    print(f"\n{'='*60}")
    print(f"  Compatibility Check Results ({elapsed:.1f}s)")
    print(f"{'='*60}")
    print(f"  Total eligible tests:   {len(tests)}")
    print(f"  LLVM native OK:         {llvm_ok}")
    print(f"  liric JIT OK:           {liric_ok}")
    print(f"  lli OK:                 {lli_ok}")
    print(f"  liric output match:     {liric_match}")
    print(f"  lli output match:       {lli_match}")
    print(f"\n  {api_path}")
    print(f"  {ll_path}")
    print(f"  {jsonl_path}")


if __name__ == "__main__":
    main()
