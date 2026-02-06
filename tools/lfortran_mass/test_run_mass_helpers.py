#!/usr/bin/env python3
"""Unit tests for mass harness extrafile compile helpers."""

from __future__ import annotations

import unittest
from pathlib import Path

from tools.lfortran_mass import run_mass


class RunMassHelperTests(unittest.TestCase):
    def test_extract_c_compile_options_include_define(self) -> None:
        opts = [
            "--cpp",
            "--realloc-lhs-arrays",
            "-I",
            "/tmp/inc",
            "-DMODE=1",
            "-U",
            "X",
            "-O3",
            "-isystem",
            "/tmp/sys",
        ]
        got = run_mass.extract_c_compile_options(opts)
        self.assertEqual(
            got,
            ["-I", "/tmp/inc", "-DMODE=1", "-U", "X", "-isystem", "/tmp/sys"],
        )

    def test_needs_fortran_cpp_from_suffix(self) -> None:
        path = Path("/tmp/example.F90")
        self.assertTrue(run_mass.needs_fortran_cpp(path, []))

    def test_needs_fortran_cpp_from_option(self) -> None:
        path = Path("/tmp/example.f90")
        self.assertTrue(run_mass.needs_fortran_cpp(path, ["--cpp"]))


if __name__ == "__main__":
    unittest.main()
