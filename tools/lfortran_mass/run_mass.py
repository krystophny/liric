#!/usr/bin/env python3
"""Run LFortran tests.toml corpus through LFortran LLVM emission and liric."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from . import classify
from . import integration_tests_cmake
from . import lfortran_tests_toml
from . import report


RE_ENTRY = re.compile(
    r"^\s*define\b(?P<prefix>.*?)@(?P<name>\"[^\"]+\"|[^\(\s]+)\s*\((?P<args>[^\)]*)\)",
    re.MULTILINE,
)
PREFERRED_ENTRIES = ["main", "_lfortran_main_program", "_QQmain"]
SUPPORTED_SIGS = {"i32", "i64", "void", "i32_argc_argv", "i64_argc_argv", "void_argc_argv"}
FORTRAN_CPP_SUFFIXES = {".F", ".F03", ".F08", ".F18", ".F90", ".F95"}
FORTRAN_FIXED_SUFFIXES = {".f", ".for", ".ftn"}
FORTRAN_FREE_SUFFIXES = {".f90", ".f95", ".f03", ".f08", ".f18"}
FORTRAN_SUFFIXES = FORTRAN_FIXED_SUFFIXES | FORTRAN_FREE_SUFFIXES | {
    x.lower() for x in FORTRAN_CPP_SUFFIXES
} | FORTRAN_CPP_SUFFIXES
C_SUFFIXES = {".c"}
CXX_SUFFIXES = {".cc", ".cpp", ".cxx", ".c++"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}
OPENMP_LIB_NAMES = (
    "libgomp.so",
    "libgomp.so.1",
    "libomp.so",
    "libomp.so.5",
    "libomp.dylib",
    "libgomp.dylib",
)
DEFAULT_OPENMP_LIB_DIRS = (
    "/usr/lib",
    "/usr/lib64",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib/aarch64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/lib/aarch64-linux-gnu",
    "/opt/homebrew/lib",
)


@dataclass(frozen=True)
class CommandResult:
    command: str
    rc: int
    stdout: str
    stderr: str
    duration_sec: float
    pid: Optional[int]


@dataclass(frozen=True)
class RunnerConfig:
    lfortran_bin: Path
    liric_bin: Path
    probe_runner: Path
    cache_dir: Path
    timeout_emit: int
    timeout_parse: int
    timeout_jit: int
    timeout_run: int
    force: bool
    runtime_libs: Tuple[str, ...]
    diag_fail_logs: bool = False
    diag_jit_coredump: bool = False


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


def load_name_list(path: Path) -> List[str]:
    names: List[str] = []
    seen: Set[str] = set()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        if name in seen:
            continue
        seen.add(name)
        names.append(name)
    return names


def select_entry_for_compat_api(
    entry: Any,
    compat_api_names: Optional[Set[str]],
) -> Tuple[bool, str]:
    if compat_api_names is None:
        return True, "included"
    if str(getattr(entry, "corpus", "unknown")) != "integration_cmake":
        return False, "compat_api_list_mode"

    name = str(getattr(entry, "name", "")).strip()
    if not name:
        return False, "compat_api_missing_name"
    if name not in compat_api_names:
        return False, "not_in_compat_api_list"
    return True, "included"


def run_cmd(cmd: List[str], cwd: Path, timeout_sec: int) -> CommandResult:
    start = time.monotonic()
    pid: Optional[int] = None
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        pid = proc.pid
        try:
            out_bytes, err_bytes = proc.communicate(timeout=timeout_sec)
            rc = proc.returncode
            stdout = out_bytes.decode("utf-8", errors="replace")
            stderr = err_bytes.decode("utf-8", errors="replace")
        except subprocess.TimeoutExpired:
            proc.kill()
            out_bytes, err_bytes = proc.communicate()
            rc = 124
            stdout = out_bytes.decode("utf-8", errors="replace")
            stderr = err_bytes.decode("utf-8", errors="replace") + "\ncommand timed out"
    except subprocess.TimeoutExpired as exc:
        rc = 124
        timeout_out = exc.stdout if isinstance(exc.stdout, bytes) else (exc.stdout or b"")
        timeout_err = exc.stderr if isinstance(exc.stderr, bytes) else (exc.stderr or b"")
        stdout = timeout_out.decode("utf-8", errors="replace")
        stderr = timeout_err.decode("utf-8", errors="replace") + "\ncommand timed out"
    except OSError as exc:
        rc = 127
        stdout = ""
        stderr = str(exc)

    duration = time.monotonic() - start
    return CommandResult(
        command=shell_join(cmd),
        rc=rc,
        stdout=stdout,
        stderr=stderr,
        duration_sec=duration,
        pid=pid,
    )


def write_diag_logs(case_dir: Path, stage: str, result: CommandResult) -> None:
    diag_dir = case_dir / "diag"
    diag_dir.mkdir(parents=True, exist_ok=True)
    (diag_dir / f"{stage}.stdout.txt").write_text(result.stdout, encoding="utf-8")
    (diag_dir / f"{stage}.stderr.txt").write_text(result.stderr, encoding="utf-8")
    meta = {
        "stage": stage,
        "command": result.command,
        "rc": result.rc,
        "duration_sec": result.duration_sec,
        "pid": result.pid,
    }
    (diag_dir / f"{stage}.meta.json").write_text(
        json.dumps(meta, ensure_ascii=True, sort_keys=True) + "\n", encoding="utf-8"
    )


def collect_jit_coredump_diag(
    cfg: RunnerConfig,
    case_dir: Path,
    jit_result: CommandResult,
    row: Dict[str, Any],
) -> None:
    if (not cfg.diag_jit_coredump) or jit_result.rc >= 0 or jit_result.pid is None:
        return
    coredumpctl = shutil.which("coredumpctl")
    if not coredumpctl:
        row["diag_coredump"] = "coredumpctl_not_found"
        return

    diag_dir = case_dir / "diag"
    diag_dir.mkdir(parents=True, exist_ok=True)
    info_cmd = [coredumpctl, "--no-pager", "info", str(jit_result.pid)]
    info_res = run_cmd(info_cmd, cwd=case_dir, timeout_sec=20)
    (diag_dir / "jit.coredump.info.txt").write_text(
        info_res.stdout + ("\n" + info_res.stderr if info_res.stderr else ""),
        encoding="utf-8",
    )
    row["diag_coredump_info_cmd"] = info_res.command
    row["diag_coredump_info_rc"] = info_res.rc

    core_path: Optional[Path] = None
    for line in (info_res.stdout + "\n" + info_res.stderr).splitlines():
        if "Storage:" not in line:
            continue
        storage = line.split("Storage:", 1)[1].strip()
        candidate = storage.split(" ", 1)[0].strip()
        if candidate and candidate != "none":
            p = Path(candidate)
            if p.exists():
                core_path = p
                break

    if core_path is not None:
        row["diag_coredump_storage"] = str(core_path)
        eu_stack = shutil.which("eu-stack")
        if eu_stack:
            stack_cmd = [
                eu_stack,
                "-a",
                "-b",
                "-m",
                "-e",
                str(cfg.probe_runner),
                "--core",
                str(core_path),
            ]
            stack_res = run_cmd(stack_cmd, cwd=case_dir, timeout_sec=20)
            (diag_dir / "jit.coredump.stack.txt").write_text(
                stack_res.stdout + ("\n" + stack_res.stderr if stack_res.stderr else ""),
                encoding="utf-8",
            )
            row["diag_coredump_stack_cmd"] = stack_res.command
            row["diag_coredump_stack_rc"] = stack_res.rc


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
    option_tokens = split_options(entry.options)
    for idx, extrafile in enumerate(entry.extrafiles):
        cmd = extrafile_compile_command(cfg, extrafile, option_tokens, case_dir, idx)
        if cmd is None:
            continue
        result = run_cmd(cmd, cwd=case_dir, timeout_sec=cfg.timeout_emit)
        if result.rc != 0:
            return result
    return None


def extrafile_compile_command(
    cfg: RunnerConfig,
    extrafile: Path,
    option_tokens: List[str],
    case_dir: Path,
    index: int,
) -> Optional[List[str]]:
    suffix = extrafile.suffix
    suffix_lower = suffix.lower()
    obj = case_dir / f"extra_{index}_{extrafile.stem}.o"

    if suffix_lower in HEADER_SUFFIXES:
        return None

    if suffix in FORTRAN_CPP_SUFFIXES or suffix_lower in FORTRAN_SUFFIXES:
        cmd = [str(cfg.lfortran_bin)]
        if suffix_lower in FORTRAN_FIXED_SUFFIXES:
            cmd.append("--fixed-form")
        if needs_fortran_cpp(extrafile, option_tokens) and "--cpp" not in option_tokens:
            cmd.append("--cpp")
        cmd.extend(option_tokens)
        cmd.extend(["-c", str(extrafile), "-o", str(obj)])
        return cmd

    if suffix_lower in C_SUFFIXES:
        compiler = resolve_compiler("CC", "cc")
        if not compiler:
            return [os.environ.get("CC", "cc")]
        cmd = [compiler]
        cmd.extend(extract_c_compile_options(option_tokens))
        cmd.extend(["-c", str(extrafile), "-o", str(obj)])
        return cmd

    if suffix_lower in CXX_SUFFIXES:
        compiler = resolve_compiler("CXX", "c++")
        if not compiler:
            return [os.environ.get("CXX", "c++")]
        cmd = [compiler]
        cmd.extend(extract_c_compile_options(option_tokens))
        cmd.extend(["-c", str(extrafile), "-o", str(obj)])
        return cmd

    return None


def resolve_compiler(env_var: str, fallback: str) -> Optional[str]:
    candidate = os.environ.get(env_var, "").strip()
    if candidate:
        if shutil.which(candidate):
            return candidate
        return None
    return shutil.which(fallback)


def needs_fortran_cpp(path: Path, option_tokens: List[str]) -> bool:
    if "--cpp" in option_tokens:
        return True
    if "-cpp" in option_tokens:
        return True
    return path.suffix in FORTRAN_CPP_SUFFIXES


def extract_c_compile_options(option_tokens: List[str]) -> List[str]:
    out: List[str] = []
    pair_flags = {"-I", "-D", "-U", "-include", "-isystem", "-idirafter", "-iquote"}
    i = 0
    while i < len(option_tokens):
        tok = option_tokens[i]
        if tok in pair_flags:
            if i + 1 < len(option_tokens):
                out.extend([tok, option_tokens[i + 1]])
                i += 2
                continue
            i += 1
            continue
        if tok.startswith("-I") or tok.startswith("-D") or tok.startswith("-U"):
            out.append(tok)
            i += 1
            continue
        i += 1
    return out


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


def choose_entrypoint(ir_text: str, run_requested: bool) -> Optional[Tuple[str, str]]:
    defs = parse_function_defs(ir_text)
    if not defs:
        return None

    by_name = {name: (ret, args) for name, ret, args in defs}

    if run_requested:
        for candidate in PREFERRED_ENTRIES:
            if candidate in by_name:
                ret, args = by_name[candidate]
                sig = signature_kind(ret, args)
                return candidate, sig
    else:
        for candidate in PREFERRED_ENTRIES:
            if candidate in by_name:
                ret, args = by_name[candidate]
                sig = signature_kind(ret, args)
                if sig in SUPPORTED_SIGS:
                    return candidate, sig
        for name, ret, args in defs:
            sig = signature_kind(ret, args)
            if sig in SUPPORTED_SIGS:
                return name, sig

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
        if re.match(
            r"^\s*i(32|64)\b[^,]*,\s*(?:i8\*\*|ptr)(?:\s|$)",
            clean_args,
        ):
            return f"{ret_type}_argc_argv" if ret_type in SUPPORTED_SIGS else "unsupported"
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


def probe_run_command(
    cfg: RunnerConfig,
    ll_path: Path,
    func: str,
    sig: str,
    ignore_retcode: bool = False,
) -> List[str]:
    cmd = [str(cfg.probe_runner)]
    for lib in cfg.runtime_libs:
        cmd.extend(["--load-lib", lib])
    if ignore_retcode:
        cmd.append("--ignore-retcode")
    cmd.extend(["--func", func, "--sig", sig, str(ll_path)])
    return cmd


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
        if cfg.diag_fail_logs:
            write_diag_logs(case_dir, "emit_prep", prep_result)
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
        if cfg.diag_fail_logs:
            write_diag_logs(case_dir, "emit", emit_result)
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
    parse_cmd = [str(cfg.liric_bin), "--dump-ir", str(ll_path)]
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
        if cfg.diag_fail_logs:
            write_diag_logs(case_dir, "parse", parse_result)
        persist_case_result(case_dir, row)
        return row

    row["parse_ok"] = True
    ep = choose_entrypoint(ir_text, bool(row["run_requested"]))
    if ep is None:
        row["stage"] = "entrypoint"
        row["classification"] = classify.UNSUPPORTED_ABI
        row["reason"] = "no define function found in emitted LLVM"
        persist_case_result(case_dir, row)
        return row

    entry_name, entry_sig = ep
    row["entrypoint"] = entry_name
    row["entry_sig"] = entry_sig

    if entry_sig not in SUPPORTED_SIGS:
        row["stage"] = "entrypoint"
        row["classification"] = classify.UNSUPPORTED_ABI
        row["reason"] = f"unsupported entry signature for JIT execution: {entry_sig}"
        persist_case_result(case_dir, row)
        return row

    row["jit_attempted"] = True
    jit_cmd = probe_run_command(
        cfg,
        ll_path,
        entry_name,
        entry_sig,
        ignore_retcode=True,
    )
    jit_result = run_cmd(jit_cmd, cwd=case_dir, timeout_sec=cfg.timeout_jit)
    row["jit_cmd"] = jit_result.command
    row["jit_rc"] = jit_result.rc
    row["jit_duration_sec"] = jit_result.duration_sec
    row["jit_pid"] = jit_result.pid

    if jit_result.rc != 0:
        row["stage"] = "jit"
        row["classification"] = classify.classify_jit_failure(jit_result.stderr, ir_text)
        row["reason"] = jit_result.stderr.strip() or "liric jit failed"
        if cfg.diag_fail_logs:
            write_diag_logs(case_dir, "jit", jit_result)
        collect_jit_coredump_diag(cfg, case_dir, jit_result, row)
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
            if cfg.diag_fail_logs:
                write_diag_logs(case_dir, "diff_ref", ref_result)
                write_diag_logs(case_dir, "diff_probe", probe_result)
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
            "(default auto-detect: <lfortran-root>/build/src/bin/lfortran, "
            "fallback <lfortran-root>/build_clean_bison/src/bin/lfortran)"
        ),
    )
    parser.add_argument(
        "--liric-bin",
        default=str((root / "build" / "liric").resolve()),
        help="Path to liric binary",
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
        default=max(1, os.cpu_count() or 1),
        help="Parallel workers (default: NCPU)",
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
    parser.add_argument(
        "--load-lib",
        action="append",
        default=[],
        help="Runtime library path to preload into liric JIT (repeatable)",
    )
    parser.add_argument(
        "--no-auto-runtime-lib",
        action="store_true",
        help=(
            "Disable automatic preload of liblfortran_runtime from the local "
            "LFortran build/install tree"
        ),
    )
    parser.add_argument(
        "--no-auto-openmp-lib",
        action="store_true",
        help=(
            "Disable automatic preload of OpenMP runtime "
            "(libgomp/libomp) when OpenMP tests are selected"
        ),
    )
    parser.add_argument(
        "--diag-fail-logs",
        action="store_true",
        help=(
            "Write per-case diagnostics logs (stdout/stderr/meta) in "
            "cache/<case_id>/diag/ for failed stages"
        ),
    )
    parser.add_argument(
        "--diag-jit-coredump",
        action="store_true",
        help=(
            "On JIT signal failures, capture coredumpctl info and eu-stack "
            "(when available) into cache/<case_id>/diag/"
        ),
    )
    parser.add_argument(
        "--compat-api-list",
        default=None,
        help=(
            "Path to compat_api*.txt list from bench_compat_check. "
            "When set, only listed integration tests are selected in list order "
            "and missing entries fail the run."
        ),
    )
    return parser.parse_args()


def ensure_exists(path: Path, label: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"{label} not found: {path}")


def resolve_default_lfortran_bin(lfortran_root: Path) -> Path:
    candidates = [
        lfortran_root / "build" / "src" / "bin" / "lfortran",
        lfortran_root / "build_clean_bison" / "src" / "bin" / "lfortran",
    ]
    for path in candidates:
        if path.exists():
            return path.resolve()
    return candidates[0].resolve()


def resolve_default_runtime_lib(lfortran_root: Path, lfortran_bin: Path) -> Optional[Path]:
    env_runtime_dir = os.environ.get("LFORTRAN_RUNTIME_LIBRARY_DIR", "").strip()
    candidate_dirs = []
    if env_runtime_dir:
        candidate_dirs.append(Path(env_runtime_dir))

    candidate_dirs.extend(
        [
            (lfortran_bin.parent / ".." / "runtime"),
            (lfortran_bin.parent / ".." / ".." / "runtime"),
            (lfortran_root / "build" / "src" / "runtime"),
            (lfortran_root / "build" / "runtime"),
        ]
    )

    seen = set()
    for dir_path in candidate_dirs:
        resolved_dir = dir_path.resolve()
        key = str(resolved_dir)
        if key in seen:
            continue
        seen.add(key)
        if not resolved_dir.exists() or not resolved_dir.is_dir():
            continue

        for name in ["liblfortran_runtime.so", "liblfortran_runtime.dylib", "liblfortran_runtime.so.0"]:
            lib = resolved_dir / name
            if lib.exists():
                return lib.resolve()

        so_candidates = sorted(resolved_dir.glob("liblfortran_runtime.so.*"))
        if so_candidates:
            return so_candidates[0].resolve()

    return None


def entry_needs_openmp_runtime(entry: Any) -> bool:
    labels = [str(x) for x in getattr(entry, "labels", [])]
    if "llvm_omp" in labels:
        return True

    tokens = split_options(str(getattr(entry, "options", "")))
    return "--openmp" in tokens or "-fopenmp" in tokens


def resolve_default_openmp_lib(lfortran_root: Path, lfortran_bin: Path) -> Optional[Path]:
    env_openmp_lib = os.environ.get("LFORTRAN_OPENMP_LIBRARY", "").strip()
    if env_openmp_lib:
        openmp_lib = Path(env_openmp_lib).resolve()
        if openmp_lib.exists():
            return openmp_lib

    env_openmp_dir = os.environ.get("LFORTRAN_OPENMP_LIBRARY_DIR", "").strip()
    candidate_dirs: List[Path] = []
    if env_openmp_dir:
        candidate_dirs.append(Path(env_openmp_dir))

    candidate_dirs.extend(
        [
            (lfortran_bin.parent / ".." / "runtime"),
            (lfortran_bin.parent / ".." / ".." / "runtime"),
            (lfortran_root / "build" / "src" / "runtime"),
            (lfortran_root / "build" / "runtime"),
            (lfortran_root / "build" / "lib"),
        ]
    )
    candidate_dirs.extend(Path(p) for p in DEFAULT_OPENMP_LIB_DIRS)

    seen = set()
    for dir_path in candidate_dirs:
        resolved_dir = dir_path.resolve()
        key = str(resolved_dir)
        if key in seen:
            continue
        seen.add(key)
        if not resolved_dir.exists() or not resolved_dir.is_dir():
            continue

        for name in OPENMP_LIB_NAMES:
            lib = resolved_dir / name
            if lib.exists():
                return lib.resolve()

        for pattern in ("libgomp.so*", "libomp.so*", "libomp*.dylib", "libgomp*.dylib"):
            candidates = sorted(resolved_dir.glob(pattern))
            if candidates:
                return candidates[0].resolve()

    return None


def selection_row(
    entry: Any,
    decision: str,
    reason: str,
    case_id: Optional[str] = None,
) -> Dict[str, Any]:
    row = {
        "corpus": str(getattr(entry, "corpus", "unknown")),
        "index": int(getattr(entry, "index", -1)),
        "filename": str(getattr(entry, "filename_raw", "")),
        "source_path": str(getattr(entry, "source_path", "")),
        "decision": decision,
        "reason": reason,
    }
    if hasattr(entry, "name"):
        row["integration_name"] = str(getattr(entry, "name"))
    if case_id is not None:
        row["case_id"] = case_id
    return row


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
        else resolve_default_lfortran_bin(lfortran_root)
    )
    compat_api_list_path = (
        Path(args.compat_api_list).resolve() if args.compat_api_list else None
    )
    compat_api_name_order: Optional[List[str]] = None
    compat_api_names: Optional[Set[str]] = None

    liric_bin = Path(args.liric_bin).resolve()
    probe_runner = Path(args.probe_runner).resolve()
    output_root = Path(args.output_root).resolve()
    cache_dir = output_root / "cache"

    ensure_exists(lfortran_root, "lfortran root")
    if not args.skip_tests_toml:
        ensure_exists(tests_toml, "tests.toml")
    if not args.skip_integration_cmake:
        ensure_exists(integration_cmake, "integration CMakeLists")
    ensure_exists(lfortran_bin, "lfortran binary")
    if compat_api_list_path is not None:
        ensure_exists(compat_api_list_path, "compat API list")
        compat_api_name_order = load_name_list(compat_api_list_path)
        if not compat_api_name_order:
            raise ValueError(f"compat API list is empty: {compat_api_list_path}")
        compat_api_names = set(compat_api_name_order)
    ensure_exists(liric_bin, "liric binary")
    ensure_exists(probe_runner, "liric_probe_runner binary")

    runtime_libs: List[str] = []
    explicit_runtime_libs = [str(Path(lib).resolve()) for lib in args.load_lib]
    runtime_libs.extend(explicit_runtime_libs)

    if not args.no_auto_runtime_lib:
        auto_runtime_lib = resolve_default_runtime_lib(lfortran_root, lfortran_bin)
        if auto_runtime_lib is not None:
            auto_runtime_lib_str = str(auto_runtime_lib)
            if auto_runtime_lib_str not in runtime_libs:
                runtime_libs.append(auto_runtime_lib_str)
        else:
            auto_runtime_lib_str = "none"
    else:
        auto_runtime_lib_str = "disabled"

    for lib in runtime_libs:
        ensure_exists(Path(lib), "runtime library")

    output_root.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)

    version_result = run_cmd([str(lfortran_bin), "--version"], cwd=output_root, timeout_sec=10)
    lfortran_version = normalize_output(version_result.stdout)
    if not lfortran_version:
        lfortran_version = normalize_output(version_result.stderr)
    if not lfortran_version:
        lfortran_version = "unknown"

    entries: List[Any] = []
    selected_before_limit: List[Any] = []
    selection_rows: List[Dict[str, Any]] = []
    skipped_non_llvm = 0
    skipped_expected_fail = 0

    if not args.skip_tests_toml:
        for entry in lfortran_tests_toml.iter_entries(tests_toml, lfortran_root):
            if not lfortran_tests_toml.is_llvm_intended(entry):
                skipped_non_llvm += 1
                selection_rows.append(
                    selection_row(entry, "skipped", "not_llvm_intended")
                )
                continue
            if (not args.include_expected_fail) and lfortran_tests_toml.is_expected_failure(entry):
                skipped_expected_fail += 1
                selection_rows.append(
                    selection_row(entry, "skipped", "expected_failure")
                )
                continue
            select_ok, select_reason = select_entry_for_compat_api(entry, compat_api_names)
            if not select_ok:
                selection_rows.append(selection_row(entry, "skipped", select_reason))
                continue
            entries.append(entry)

    if not args.skip_integration_cmake:
        for entry in integration_tests_cmake.iter_integration_entries(integration_cmake):
            if not integration_tests_cmake.is_llvm_intended(entry):
                skipped_non_llvm += 1
                selection_rows.append(
                    selection_row(entry, "skipped", "not_llvm_intended")
                )
                continue
            if (not args.include_expected_fail) and integration_tests_cmake.is_expected_failure(entry):
                skipped_expected_fail += 1
                selection_rows.append(
                    selection_row(entry, "skipped", "expected_failure")
                )
                continue
            select_ok, select_reason = select_entry_for_compat_api(entry, compat_api_names)
            if not select_ok:
                selection_rows.append(selection_row(entry, "skipped", select_reason))
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
            selection_rows.append(selection_row(entry, "skipped", "duplicate_case"))
            continue
        seen_keys.add(key)
        deduped_entries.append(entry)

    if compat_api_name_order is not None:
        by_name: Dict[str, Any] = {}
        for entry in deduped_entries:
            name = str(getattr(entry, "name", "")).strip()
            if name and name not in by_name:
                by_name[name] = entry

        missing_names = [name for name in compat_api_name_order if name not in by_name]
        if missing_names:
            preview = ", ".join(missing_names[:20])
            raise ValueError(
                f"compat API list contains {len(missing_names)} names not selected: {preview}"
            )

        deduped_entries = [by_name[name] for name in compat_api_name_order]

    selected_before_limit = deduped_entries
    entries = selected_before_limit
    if args.limit > 0:
        for entry in entries[args.limit:]:
            selection_rows.append(selection_row(entry, "skipped", "limit"))
        entries = entries[: args.limit]

    manifest_rows = [
        entry.to_manifest_record(lfortran_version=lfortran_version) for entry in entries
    ]
    for entry, manifest_row in zip(entries, manifest_rows):
        selection_rows.append(
            selection_row(entry, "selected", "included", case_id=manifest_row["case_id"])
        )

    manifest_path = output_root / "manifest_tests_toml.jsonl"
    selection_path = output_root / "selection_decisions.jsonl"
    lfortran_tests_toml.write_jsonl(manifest_path, manifest_rows)
    report.write_jsonl(selection_path, selection_rows)

    openmp_needed = any(entry_needs_openmp_runtime(entry) for entry in entries)
    if args.no_auto_openmp_lib:
        auto_openmp_lib_str = "disabled"
    elif not openmp_needed:
        auto_openmp_lib_str = "not-needed"
    else:
        auto_openmp_lib = resolve_default_openmp_lib(lfortran_root, lfortran_bin)
        if auto_openmp_lib is not None:
            auto_openmp_lib_str = str(auto_openmp_lib)
            if auto_openmp_lib_str not in runtime_libs:
                runtime_libs.append(auto_openmp_lib_str)
        else:
            auto_openmp_lib_str = "none"

    cfg = RunnerConfig(
        lfortran_bin=lfortran_bin,
        liric_bin=liric_bin,
        probe_runner=probe_runner,
        cache_dir=cache_dir,
        timeout_emit=args.timeout_emit,
        timeout_parse=args.timeout_parse,
        timeout_jit=args.timeout_jit,
        timeout_run=args.timeout_run,
        force=args.force,
        runtime_libs=tuple(runtime_libs),
        diag_fail_logs=bool(args.diag_fail_logs),
        diag_jit_coredump=bool(args.diag_jit_coredump),
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
    summary_json_path = output_root / "summary.json"
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
        selection_rows=selection_rows,
    )
    report.write_summary_json(summary_json_path, summary)
    report.write_summary_md(summary_path, summary)
    report.write_failures_csv(failures_path, processed_rows)

    if args.update_baseline:
        report.write_jsonl(baseline_path, processed_rows)

    print(f"manifest: {manifest_path}")
    print(f"selection: {selection_path}")
    print(f"results: {results_path}")
    print(f"summary_json: {summary_json_path}")
    print(f"summary: {summary_path}")
    print(f"failures: {failures_path}")
    print(f"filtered_non_llvm: {skipped_non_llvm}")
    print(f"filtered_expected_fail: {skipped_expected_fail}")
    if compat_api_list_path is not None and compat_api_name_order is not None:
        print(f"compat_api_list: {compat_api_list_path}")
        print(f"compat_api_requested: {len(compat_api_name_order)}")
    print(f"auto_runtime_lib: {auto_runtime_lib_str}")
    print(f"auto_openmp_lib: {auto_openmp_lib_str}")
    print(f"deduped_cases: {deduped_count}")

    ok, message = report.gate_result(summary)
    print(f"gate: {'pass' if ok else 'fail'} ({message})")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
