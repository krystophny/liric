#!/usr/bin/env python
"""Run lfortran reference tests with liric-aware IR comparison policy.

All backends execute normally.  For backends whose textual output
legitimately differs under liric (LLVM IR text, debug-info stacktraces),
commands still run (catching crashes) but output is not compared against
reference files.

Usage (from lfortran source root):
    python <liric>/tools/lfortran_ref_test_liric.py [run_tests.py args...]

Environment:
    LIRIC_REF_SKIP_COMPARE  comma-separated backend names to skip comparison
                            (default: llvm,run_dbg)
"""

import importlib
import logging
import os
import sys

# ---- configuration --------------------------------------------------------

SKIP_COMPARE = frozenset(
    os.environ.get(
        "LIRIC_REF_SKIP_COMPARE", "llvm,run_dbg"
    ).split(",")
)

# ---- bootstrap lfortran imports -------------------------------------------

ROOT_DIR = os.path.abspath(os.getcwd())
sys.path.insert(0, os.path.join(ROOT_DIR, "src", "libasr"))
sys.path.insert(0, ROOT_DIR)

import compiler_tester.tester as tester

log = logging.getLogger("compiler_tester.tester")

_original_run_test = tester.run_test


def _patched_run_test(
    testname,
    basename,
    cmd,
    infile,
    update_reference=False,
    verify_hash=False,
    extra_args=None,
):
    if basename in SKIP_COMPARE:
        s = f"{testname} * {basename}"
        bn = tester.bname(basename, cmd, infile)
        infile_path = os.path.join("tests", infile)
        jo = tester.run(
            bn, cmd, os.path.join("tests", "output"),
            infile=infile_path, extra_args=extra_args,
        )
        if not os.path.exists(jo):
            raise tester.RunException(
                f"IR smoke: {s}: command produced no output json"
            )
        if tester.no_color:
            log.info(f"{s} PASS (IR compare skipped)")
        else:
            log.info(
                f"{s} "
                + tester.color(tester.fg.green)
                + tester.color(tester.style.bold)
                + "✓"
                + tester.color(tester.fg.reset)
                + tester.color(tester.style.reset)
                + " (IR compare skipped)"
            )
        return

    _original_run_test(
        testname, basename, cmd, infile,
        update_reference=update_reference,
        verify_hash=verify_hash,
        extra_args=extra_args,
    )


# ---- patch and run --------------------------------------------------------

# Patch in tester module
tester.run_test = _patched_run_test

# Import run_tests which binds run_test from tester at import time;
# we need to patch it there too.
import run_tests  # noqa: E402

run_tests.run_test = _patched_run_test

# Run
tester.tester_main("LFortran", run_tests.single_test)
