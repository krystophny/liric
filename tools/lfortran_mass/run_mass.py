#!/usr/bin/env python3
"""Run LFortran tests.toml corpus through LFortran LLVM emission and liric."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from . import classify
from . import integration_tests_cmake
from . import lfortran_tests_toml
from . import report


RE_ENTRY = re.compile(
    r"^\s*define\b(?P<prefix>.*?)@(?P<name>\"[^\"]+\"|[^\(\s]+)\s*\((?P<args>[^\)]*)\)",
    re.MULTILINE,
)
PREFERRED_ENTRIES = ["main", "_lfortran_main_program", "_QQmain"]
SUPPORTED_SIGS = {"i32", "i64", "void"}


@dataclass(frozen=True)
class CommandResult:
    command: str
    rc: int
    stdout: str
    stderr: str
    duration_sec: float


@dataclass(frozen=True)
class RunnerConfig:
    lfortran_bin: Path
    liric_cli: Path
    probe_runner: Path
    cache_dir: Path
    timeout_emit: int
    timeout_parse: int
    timeout_jit: int
    timeout_run: int
    force: bool


def split_options(options: str) -> List[str]:
    if not options.strip():
        return []
    return shlex.split(options)


def shell_join(cmd: List[str]) -> str:
    return " ".join(shlex.quote(part) for part in cmd)


def normalize_output(text: str) -> str:
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    stripped = [line.rstrip() for line in lines]
    while stripped and stripped[-1] == "":
        stripped.pop()
    return "\n".join(stripped)


def run_cmd(cmd: List[str], cwd: Path, timeout_sec: int) -> CommandResult:
    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            capture_output=True,
            timeout=timeout_sec,
            check=False,
        )
        rc = proc.returncode
        stdout = proc.stdout.decode("utf-8", errors="replace")
        stderr = proc.stderr.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as exc:
        rc = 124
        timeout_out = exc.stdout if isinstance(exc.stdout, bytes) else (exc.stdout or b"")
        timeout_err = exc.stderr if isinstance(exc.stderr, bytes) else (exc.stderr or b"")
        stdout = timeout_out.decode("utf-8", errors="replace")
        stderr = timeout_err.decode("utf-8", errors="replace") + "\ncommand timed out"

    duration = time.monotonic() - start
    return CommandResult(
        command=shell_join(cmd),
        rc=rc,
        stdout=stdout,
        stderr=stderr,
        duration_sec=duration,
    )


def choose_emit_recipe(entry: Any) -> str:
    if entry.pass_with_llvm and entry.pass_name:
        return "pass_with_llvm"
    if entry.asr_implicit_interface_and_typing_with_llvm:
        return "asr_implicit_interface_and_typing_with_llvm"
    if entry.llvm:
        return "llvm"
    return "fallback_llvm"


def emit_command(
    lfortran_bin: Path,
    entry: Any,
    output_ll: Path,
) -> List[str]:
    recipe = choose_emit_recipe(entry)
    cmd = [str(lfortran_bin)]

    if recipe == "pass_with_llvm":
        cmd.extend(["--no-color", "--show-llvm", f"--pass={entry.pass_name}"])
        if entry.fast:
            cmd.append("--fast")
    elif recipe == "asr_implicit_interface_and_typing_with_llvm":
        cmd.extend(["--show-llvm", "--implicit-typing", "--implicit-interface"])
    else:
        cmd.extend(["--no-color", "--show-llvm"])

    if entry.source_path.suffix.lower() == ".f":
        cmd.append("--fixed-form")

    cmd.extend(split_options(entry.options))
    cmd.extend([str(entry.source_path), "-o", str(output_ll)])
    return cmd


def compile_extrafiles(
    cfg: RunnerConfig,
    entry: Any,
    case_dir: Path,
) -> Optional[CommandResult]:
    for extrafile in entry.extrafiles:
        cmd = [str(cfg.lfortran_bin)]
        if extrafile.suffix.lower() == ".f":
            cmd.append("--fixed-form")
        cmd.extend(["-c", str(extrafile)])
        result = run_cmd(cmd, cwd=case_dir, timeout_sec=cfg.timeout_emit)
        if result.rc != 0:
            return result
    return None


def parse_function_defs(ir_text: str) -> List[Tuple[str, str, str]]:
    defs: List[Tuple[str, str, str]] = []
    for match in RE_ENTRY.finditer(ir_text):
        prefix = match.group("prefix").strip()
        name = match.group("name").strip()
        args = match.group("args").strip()

        if name.startswith('"') and name.endswith('"'):
            name = name[1:-1]

        tokens = prefix.split()
        if not tokens:
            continue
        ret_type = tokens[-1]
        defs.append((name, ret_type, args))

    return defs


def choose_entrypoint(ir_text: str) -> Optional[Tuple[str, str]]:
    defs = parse_function_defs(ir_text)
    if not defs:
        return None

    by_name = {name: (ret, args) for name, ret, args in defs}
    for candidate in PREFERRED_ENTRIES:
        if candidate in by_name:
            ret, args = by_name[candidate]
            sig = signature_kind(ret, args)
            return candidate, sig

    first = defs[0]
    return first[0], signature_kind(first[1], first[2])


def signature_kind(ret_type: str, args: str) -> str:
    clean_args = args.strip()
    if clean_args and clean_args != "void":
        return "unsupported"
    if ret_type in SUPPORTED_SIGS:
        return ret_type
    return "unsupported"


def reference_run_command(
    cfg: RunnerConfig,
    entry: Any,
) -> List[str]:
    cmd = [str(cfg.lfortran_bin)]
    if entry.run_with_dbg:
        cmd.extend([str(entry.source_path), "-g", "--no-color"])
    else:
        cmd.extend(["--no-color", str(entry.source_path)])
    cmd.extend(split_options(entry.options))
    return cmd


def probe_run_command(cfg: RunnerConfig, ll_path: Path, func: str, sig: str) -> List[str]:
    return [str(cfg.probe_runner), "--func", func, "--sig", sig, str(ll_path)]


def init_result_row(
    manifest_row: Dict[str, Any],
    entry: Any,
) -> Dict[str, Any]:
    row = dict(manifest_row)
    row.update(
        {
            "classification": classify.INFRA_FAIL,
            "stage": "start",
            "reason": "",
            "emit_attempted": False,
            "emit_ok": False,
            "parse_attempted": False,
            "parse_ok": False,
            "jit_attempted": False,
            "jit_ok": False,
            "differential_attempted": False,
            "differential_ok": False,
            "differential_match": None,
            "entrypoint": None,
            "entry_sig": None,
            "run_requested": bool(entry.run or entry.run_with_dbg),
        }
    )
    return row


def persist_case_result(case_dir: Path, row: Dict[str, Any]) -> None:
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / "result.json").write_text(
        json.dumps(row, ensure_ascii=True, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def load_cached_case_result(case_dir: Path) -> Optional[Dict[str, Any]]:
    result_path = case_dir / "result.json"
    if not result_path.exists():
        return None
    try:
        return json.loads(result_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def process_case(
    entry: Any,
    manifest_row: Dict[str, Any],
    cfg: RunnerConfig,
) -> Dict[str, Any]:
    case_dir = cfg.cache_dir / manifest_row["case_id"]
    if not cfg.force:
        cached = load_cached_case_result(case_dir)
        if cached is not None:
            return cached

    row = init_result_row(manifest_row, entry)
    case_dir.mkdir(parents=True, exist_ok=True)
    ll_path = case_dir / "raw.ll"

    if not entry.source_path.exists():
        row["stage"] = "manifest"
        row["reason"] = f"source file not found: {entry.source_path}"
        persist_case_result(case_dir, row)
        return row

    missing_extra = [str(p) for p in entry.extrafiles if not p.exists()]
    if missing_extra:
        row["stage"] = "manifest"
        row["reason"] = f"missing extrafiles: {', '.join(missing_extra)}"
        persist_case_result(case_dir, row)
        return row

    prep_result = compile_extrafiles(cfg, entry, case_dir)
    if prep_result is not None:
        row["stage"] = "emit_prep"
        row["classification"] = classify.LFORTRAN_EMIT_FAIL
        row["reason"] = prep_result.stderr.strip() or "failed to compile extrafiles"
        row["emit_prep_rc"] = prep_result.rc
        row["emit_prep_cmd"] = prep_result.command
        persist_case_result(case_dir, row)
        return row

    row["emit_attempted"] = True
    emit_cmd = emit_command(cfg.lfortran_bin, entry, ll_path)
    emit_result = run_cmd(emit_cmd, cwd=case_dir, timeout_sec=cfg.timeout_emit)
    row["emit_cmd"] = emit_result.command
    row["emit_rc"] = emit_result.rc
    row["emit_duration_sec"] = emit_result.duration_sec

    if emit_result.rc != 0:
        row["stage"] = "emit"
        row["classification"] = classify.LFORTRAN_EMIT_FAIL
        row["reason"] = emit_result.stderr.strip() or "lfortran llvm emission failed"
        persist_case_result(case_dir, row)
        return row

    row["emit_ok"] = True
    if not ll_path.exists():
        stdout_ir = emit_result.stdout
        if stdout_ir.strip():
            ll_path.write_text(stdout_ir, encoding="utf-8")
        else:
            row["stage"] = "emit"
            row["classification"] = classify.LFORTRAN_EMIT_FAIL
            row["reason"] = "lfortran emitted no llvm output file"
            persist_case_result(case_dir, row)
            return row
    if ll_path.stat().st_size == 0:
        stdout_ir = emit_result.stdout
        if stdout_ir.strip():
            ll_path.write_text(stdout_ir, encoding="utf-8")
        else:
            row["stage"] = "emit"
            row["classification"] = classify.LFORTRAN_EMIT_FAIL
            row["reason"] = "lfortran emitted empty llvm output"
            persist_case_result(case_dir, row)
            return row

    ir_text = ll_path.read_text(encoding="utf-8", errors="replace")

    row["parse_attempted"] = True
    parse_cmd = [str(cfg.liric_cli), "--dump-ir", str(ll_path)]
    parse_result = run_cmd(parse_cmd, cwd=case_dir, timeout_sec=cfg.timeout_parse)
    row["parse_cmd"] = parse_result.command
    row["parse_rc"] = parse_result.rc
    row["parse_duration_sec"] = parse_result.duration_sec

    if parse_result.rc != 0:
        row["stage"] = "parse"
        row["classification"] = classify.classify_parse_failure(
            parse_result.stderr, ir_text
        )
        row["reason"] = parse_result.stderr.strip() or "liric parse failed"
        persist_case_result(case_dir, row)
        return row

    row["parse_ok"] = True
    ep = choose_entrypoint(ir_text)
    if ep is None:
        row["stage"] = "entrypoint"
        row["classification"] = classify.UNSUPPORTED_ABI
        row["reason"] = "no define function found in emitted LLVM"
        persist_case_result(case_dir, row)
        return row

    entry_name, entry_sig = ep
    row["entrypoint"] = entry_name
    row["entry_sig"] = entry_sig

    row["jit_attempted"] = True
    jit_cmd = [str(cfg.liric_cli), "--jit", "--func", entry_name, str(ll_path)]
    jit_result = run_cmd(jit_cmd, cwd=case_dir, timeout_sec=cfg.timeout_jit)
    row["jit_cmd"] = jit_result.command
    row["jit_rc"] = jit_result.rc
    row["jit_duration_sec"] = jit_result.duration_sec

    if jit_result.rc != 0:
        row["stage"] = "jit"
        row["classification"] = classify.classify_jit_failure(jit_result.stderr, ir_text)
        row["reason"] = jit_result.stderr.strip() or "liric jit failed"
        persist_case_result(case_dir, row)
        return row

    row["jit_ok"] = True
    row["classification"] = classify.PASS
    row["stage"] = "jit"

    if entry.run or entry.run_with_dbg:
        row["differential_attempted"] = True
        if entry_sig not in SUPPORTED_SIGS:
            row["classification"] = classify.UNSUPPORTED_ABI
            row["stage"] = "differential_prepare"
            row["reason"] = f"unsupported entry signature for runner: {entry_sig}"
            persist_case_result(case_dir, row)
            return row

        ref_cmd = reference_run_command(cfg, entry)
        ref_result = run_cmd(ref_cmd, cwd=case_dir, timeout_sec=cfg.timeout_run)
        row["diff_ref_cmd"] = ref_result.command
        row["diff_ref_rc"] = ref_result.rc

        probe_cmd = probe_run_command(cfg, ll_path, entry_name, entry_sig)
        probe_result = run_cmd(probe_cmd, cwd=case_dir, timeout_sec=cfg.timeout_run)
        row["diff_probe_cmd"] = probe_result.command
        row["diff_probe_rc"] = probe_result.rc

        ref_out = normalize_output(ref_result.stdout)
        probe_out = normalize_output(probe_result.stdout)
        ref_err = normalize_output(ref_result.stderr)
        probe_err = normalize_output(probe_result.stderr)

        rc_match = ref_result.rc == probe_result.rc
        out_match = ref_out == probe_out
        err_match = ref_err == probe_err

        row["differential_ok"] = True
        row["differential_match"] = rc_match and out_match and err_match
        row["differential_rc_match"] = rc_match
        row["differential_stdout_match"] = out_match
        row["differential_stderr_match"] = err_match

        if not row["differential_match"]:
            row["classification"] = classify.MISMATCH
            row["stage"] = "differential"
            row["reason"] = "reference and liric runtime outputs differ"
        else:
            row["classification"] = classify.PASS
            row["stage"] = "differential"

    persist_case_result(case_dir, row)
    return row


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[2]
    default_lfortran_root = (root.parent / "lfortran").resolve()
    default_output_root = Path("/tmp/liric_lfortran_mass")

    parser = argparse.ArgumentParser(
        description="Run LFortran tests.toml corpus through liric parse/jit checks"
    )
    parser.add_argument(
        "--lfortran-root",
        default=str(default_lfortran_root),
        help="LFortran repo root (default: ../lfortran)",
    )
    parser.add_argument(
        "--tests-toml",
        default=None,
        help="Path to tests.toml (default: <lfortran-root>/tests/tests.toml)",
    )
    parser.add_argument(
        "--integration-cmake",
        default=None,
        help=(
            "Path to integration_tests/CMakeLists.txt "
            "(default: <lfortran-root>/integration_tests/CMakeLists.txt)"
        ),
    )
    parser.add_argument(
        "--lfortran-bin",
        default=None,
        help=(
            "Path to lfortran binary "
            "(default: <lfortran-root>/build_clean_bison/src/bin/lfortran)"
        ),
    )
    parser.add_argument(
        "--liric-cli",
        default=str((root / "build" / "liric_cli").resolve()),
        help="Path to liric_cli binary",
    )
    parser.add_argument(
        "--probe-runner",
        default=str((root / "build" / "liric_probe_runner").resolve()),
        help="Path to liric_probe_runner binary",
    )
    parser.add_argument(
        "--output-root",
        default=str(default_output_root),
        help="Output directory root",
    )
    parser.add_argument(
        "--baseline",
        default=None,
        help="Optional baseline results JSONL for regression comparison",
    )
    parser.add_argument(
        "--update-baseline",
        action="store_true",
        help="Write current results to baseline file",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, (os.cpu_count() or 2) - 1),
        help="Parallel workers (default: NCPU-1)",
    )
    parser.add_argument(
        "--skip-tests-toml",
        action="store_true",
        help="Do not include tests/tests.toml corpus",
    )
    parser.add_argument(
        "--skip-integration-cmake",
        action="store_true",
        help="Do not include integration_tests/CMakeLists corpus",
    )
    parser.add_argument(
        "--include-expected-fail",
        action="store_true",
        help="Include expected-failure tests (default is excluded)",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Optional limit of processed test entries (0 means all)",
    )
    parser.add_argument("--timeout-emit", type=int, default=20)
    parser.add_argument("--timeout-parse", type=int, default=10)
    parser.add_argument("--timeout-jit", type=int, default=10)
    parser.add_argument("--timeout-run", type=int, default=15)
    parser.add_argument(
        "--force",
        action="store_true",
        help="Ignore cache and recompute each case",
    )
    return parser.parse_args()


def ensure_exists(path: Path, label: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{label} not found: {path}")


def main() -> int:
    args = parse_args()

    lfortran_root = Path(args.lfortran_root).resolve()
    tests_toml = (
        Path(args.tests_toml).resolve()
        if args.tests_toml
        else (lfortran_root / "tests" / "tests.toml").resolve()
    )
    integration_cmake = (
        Path(args.integration_cmake).resolve()
        if args.integration_cmake
        else (lfortran_root / "integration_tests" / "CMakeLists.txt").resolve()
    )
    lfortran_bin = (
        Path(args.lfortran_bin).resolve()
        if args.lfortran_bin
        else (lfortran_root / "build_clean_bison" / "src" / "bin" / "lfortran").resolve()
    )

    liric_cli = Path(args.liric_cli).resolve()
    probe_runner = Path(args.probe_runner).resolve()
    output_root = Path(args.output_root).resolve()
    cache_dir = output_root / "cache"

    ensure_exists(lfortran_root, "lfortran root")
    if not args.skip_tests_toml:
        ensure_exists(tests_toml, "tests.toml")
    if not args.skip_integration_cmake:
        ensure_exists(integration_cmake, "integration CMakeLists")
    ensure_exists(lfortran_bin, "lfortran binary")
    ensure_exists(liric_cli, "liric_cli binary")
    ensure_exists(probe_runner, "liric_probe_runner binary")

    output_root.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)

    version_result = run_cmd([str(lfortran_bin), "--version"], cwd=output_root, timeout_sec=10)
    lfortran_version = normalize_output(version_result.stdout)
    if not lfortran_version:
        lfortran_version = normalize_output(version_result.stderr)
    if not lfortran_version:
        lfortran_version = "unknown"

    entries: List[Any] = []
    skipped_non_llvm = 0
    skipped_expected_fail = 0

    if not args.skip_tests_toml:
        for entry in lfortran_tests_toml.iter_entries(tests_toml, lfortran_root):
            if not lfortran_tests_toml.is_llvm_intended(entry):
                skipped_non_llvm += 1
                continue
            if (not args.include_expected_fail) and lfortran_tests_toml.is_expected_failure(entry):
                skipped_expected_fail += 1
                continue
            entries.append(entry)

    if not args.skip_integration_cmake:
        for entry in integration_tests_cmake.iter_integration_entries(integration_cmake):
            if not integration_tests_cmake.is_llvm_intended(entry):
                skipped_non_llvm += 1
                continue
            if (not args.include_expected_fail) and integration_tests_cmake.is_expected_failure(entry):
                skipped_expected_fail += 1
                continue
            entries.append(entry)

    deduped_entries: List[Any] = []
    seen_keys = set()
    deduped_count = 0
    for entry in entries:
        key = (
            str(entry.source_path),
            entry.options,
            tuple(str(x) for x in entry.extrafiles),
            choose_emit_recipe(entry),
            bool(entry.run),
            bool(entry.run_with_dbg),
        )
        if key in seen_keys:
            deduped_count += 1
            continue
        seen_keys.add(key)
        deduped_entries.append(entry)

    entries = deduped_entries
    if args.limit > 0:
        entries = entries[: args.limit]

    manifest_rows = [
        entry.to_manifest_record(lfortran_version=lfortran_version) for entry in entries
    ]

    manifest_path = output_root / "manifest_tests_toml.jsonl"
    lfortran_tests_toml.write_jsonl(manifest_path, manifest_rows)

    cfg = RunnerConfig(
        lfortran_bin=lfortran_bin,
        liric_cli=liric_cli,
        probe_runner=probe_runner,
        cache_dir=cache_dir,
        timeout_emit=args.timeout_emit,
        timeout_parse=args.timeout_parse,
        timeout_jit=args.timeout_jit,
        timeout_run=args.timeout_run,
        force=args.force,
    )

    work_items = list(zip(entries, manifest_rows))
    processed_rows: List[Dict[str, Any]] = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = [
            pool.submit(process_case, entry, manifest_row, cfg)
            for entry, manifest_row in work_items
        ]
        for future in concurrent.futures.as_completed(futures):
            processed_rows.append(future.result())

    processed_rows.sort(key=lambda row: int(row.get("index", 0)))

    results_path = output_root / "results.jsonl"
    summary_path = output_root / "summary.md"
    failures_path = output_root / "failures.csv"

    report.write_jsonl(results_path, processed_rows)

    baseline_path = (
        Path(args.baseline).resolve()
        if args.baseline
        else (output_root / "baseline.jsonl")
    )
    baseline_rows = report.load_jsonl(baseline_path)

    summary = report.summarize(
        manifest_total=len(manifest_rows),
        processed=processed_rows,
        baseline=baseline_rows,
    )
    report.write_summary_md(summary_path, summary)
    report.write_failures_csv(failures_path, processed_rows)

    if args.update_baseline:
        report.write_jsonl(baseline_path, processed_rows)

    print(f"manifest: {manifest_path}")
    print(f"results: {results_path}")
    print(f"summary: {summary_path}")
    print(f"failures: {failures_path}")
    print(f"filtered_non_llvm: {skipped_non_llvm}")
    print(f"filtered_expected_fail: {skipped_expected_fail}")
    print(f"deduped_cases: {deduped_count}")

    ok, message = report.gate_result(summary)
    print(f"gate: {'pass' if ok else 'fail'} ({message})")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
