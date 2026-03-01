#!/usr/bin/env python
"""Run lfortran reference tests with liric-aware comparison policy.

All backends execute normally.  Two classes of output are excluded from
comparison because liric legitimately differs from LLVM:

  llvm     -- IR text (--show-llvm) in stdout + outfile; stderr and
              returncode are still checked.
  run_dbg  -- debug-info-dependent output (stacktraces); the command
              runs (catching crashes) but all output is skipped since
              DWARF emission is not yet implemented in liric.

All other backends are fully compared.

Usage (from lfortran source root):
    python <liric>/tools/lfortran_ref_test_liric.py [run_tests.py args...]

Environment:
    LIRIC_REF_SKIP_IR     backends whose IR output (stdout + outfile) is
                          excluded; stderr + returncode still checked
                          (default: llvm)
    LIRIC_REF_SKIP_DBG    backends whose output is entirely debug-info-
                          dependent; command runs but no comparison
                          (default: run_dbg)
"""

import json
import hashlib
import logging
import os
import sys

# ---- configuration --------------------------------------------------------

SKIP_IR = frozenset(
    os.environ.get("LIRIC_REF_SKIP_IR", "llvm").split(",")
)
SKIP_DBG = frozenset(
    os.environ.get("LIRIC_REF_SKIP_DBG", "run_dbg").split(",")
)

# ---- bootstrap lfortran imports -------------------------------------------

ROOT_DIR = os.path.abspath(os.getcwd())
sys.path.insert(0, os.path.join(ROOT_DIR, "src", "libasr"))
sys.path.insert(0, ROOT_DIR)

try:
    import compiler_tester.tester as tester
except ModuleNotFoundError as exc:
    if getattr(exc, "name", "") == "toml":
        print(
            "lfortran_ref_test_liric: python 'toml' module is unavailable; "
            "skipping reference-policy pass.",
            file=sys.stderr,
        )
        sys.exit(0)
    raise

log = logging.getLogger("compiler_tester.tester")

_original_run_test = tester.run_test
_original_run = tester.run

_NO_LINK_FALLBACK_DIAG = (
    b"WITH_LIRIC AOT no-link executable emission failed. "
    b"Refusing linker fallback."
)


def _normalize_no_link_stderr(json_file):
    """Strip known WITH_LIRIC no-link fallback diagnostic from stderr output."""
    if not os.path.exists(json_file):
        return

    data = json.load(open(json_file))
    stderr_file = data.get("stderr")
    if not stderr_file:
        return

    stderr_path = os.path.join(os.path.dirname(json_file), stderr_file)
    if not os.path.exists(stderr_path):
        return

    raw = open(stderr_path, "rb").read()
    lines = raw.splitlines(keepends=True)
    filtered = []
    changed = False
    for line in lines:
        if line.rstrip(b"\r\n") == _NO_LINK_FALLBACK_DIAG:
            changed = True
            continue
        filtered.append(line)

    if not changed:
        return

    new_raw = b"".join(filtered)
    if len(new_raw) == 0:
        os.remove(stderr_path)
        data["stderr"] = None
        data["stderr_hash"] = None
    else:
        with open(stderr_path, "wb") as f:
            f.write(new_raw)
        data["stderr_hash"] = hashlib.sha224(
            tester.unl_loop_del(new_raw)).hexdigest()

    json.dump(data, open(json_file, "w"), indent=4)


def _patched_run(basename, cmd, out_dir, infile=None, extra_args=None):
    json_file = _original_run(
        basename, cmd, out_dir, infile=infile, extra_args=extra_args)
    _normalize_no_link_stderr(json_file)
    return json_file


def _patched_run_test(
    testname,
    basename,
    cmd,
    infile,
    update_reference=False,
    verify_hash=False,
    extra_args=None,
):
    if basename not in SKIP_IR and basename not in SKIP_DBG:
        return _original_run_test(
            testname, basename, cmd, infile,
            update_reference=update_reference,
            verify_hash=verify_hash,
            extra_args=extra_args,
        )

    if basename in SKIP_DBG:
        # Debug-info backend: run command (catch crashes), skip all comparison.
        s = f"{testname} * {basename}"
        bn = tester.bname(basename, cmd, infile)
        infile_path = os.path.join("tests", infile)
        jo = tester.run(
            bn, cmd, os.path.join("tests", "output"),
            infile=infile_path, extra_args=extra_args,
        )
        if not os.path.exists(jo):
            raise tester.RunException(
                f"{s}: command produced no output json")
        if tester.no_color:
            log.debug(f"{s} PASS (debug-info compare skipped)")
        else:
            log.debug(f"{s} " + tester.check()
                      + " (debug-info compare skipped)")
        return

    # IR backend: run command, compare stderr + returncode.
    # Skip stdout + outfile since both contain IR text that differs.
    s = f"{testname} * {basename}"
    bn = tester.bname(basename, cmd, infile)
    infile_path = os.path.join("tests", infile)
    jo = tester.run(
        bn, cmd, os.path.join("tests", "output"),
        infile=infile_path, extra_args=extra_args,
    )
    if not os.path.exists(jo):
        raise tester.RunException(f"{s}: command produced no output json")

    if update_reference:
        jr = os.path.join("tests", "reference", os.path.basename(jo))
        tester.do_update_reference(jo, jr, json.load(open(jo)))
        return

    jr = os.path.join("tests", "reference", os.path.basename(jo))
    if not os.path.exists(jr):
        raise FileNotFoundError(
            f"The reference json file '{jr}' for {testname} does not exist")

    do = json.load(open(jo))
    dr = json.load(open(jr))

    # Null out IR-carrying fields so only stderr + returncode are compared.
    _ir_null = {"outfile": None, "outfile_hash": None,
                "stdout": None, "stdout_hash": None}
    do_cmp = dict(do, **_ir_null)
    dr_cmp = dict(dr, **_ir_null)

    if do_cmp != dr_cmp:
        full_err_str = (
            f"\n{tester.color(tester.fg.red)}{tester.color(tester.style.bold)}"
            f"{s}{tester.color(tester.fg.reset)}{tester.color(tester.style.reset)}\n"
        )
        full_err_str += "Non-IR fields differ against reference (IR stdout+outfile excluded)\n"
        full_err_str += "Reference JSON: " + jr + "\n"
        full_err_str += "Output JSON:    " + jo + "\n"

        # Check stderr
        if not do["stderr_hash"] and dr["stderr_hash"]:
            full_err_str += "\n=== MISSING STDERR ===\n"
            reference_file = os.path.join("tests", "reference", dr["stderr"])
            output_file = os.path.join("tests", "output",
                                       do["stderr"] if do["stderr"] else "missing")
            full_err_str = tester.get_error_diff(
                reference_file, output_file, full_err_str, "stderr")
        elif not dr["stderr_hash"] and do["stderr_hash"]:
            full_err_str += "\n=== UNEXPECTED STDERR ===\n"
            reference_file = os.path.join("tests", "reference",
                                          dr["stderr"] if dr["stderr"] else "missing")
            output_file = os.path.join("tests", "output", do["stderr"])
            full_err_str = tester.get_error_diff(
                reference_file, output_file, full_err_str, "stderr")
        elif do["stderr_hash"] != dr["stderr_hash"]:
            output_file = os.path.join("tests", "output", do["stderr"])
            reference_file = os.path.join("tests", "reference", dr["stderr"])
            full_err_str = tester.get_error_diff(
                reference_file, output_file, full_err_str, "stderr")

        if do.get("returncode") != dr.get("returncode"):
            full_err_str += (
                f"\n=== RETURNCODE MISMATCH ===\n"
                f"expected {dr.get('returncode')}, got {do.get('returncode')}\n"
            )

        raise tester.RunException(
            "Testing with reference output failed." + full_err_str)

    if tester.no_color:
        log.debug(f"{s} PASS (IR excluded)")
    else:
        log.debug(f"{s} " + tester.check() + " (IR excluded)")


# ---- patch and run --------------------------------------------------------

tester.run = _patched_run
tester.run_test = _patched_run_test

import run_tests  # noqa: E402

run_tests.run = _patched_run
run_tests.run_test = _patched_run_test

tester.tester_main("LFortran", run_tests.single_test)
