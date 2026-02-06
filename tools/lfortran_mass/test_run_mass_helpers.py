#!/usr/bin/env python3
"""Unit tests for mass harness extrafile compile helpers."""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

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

    def test_resolve_default_lfortran_bin_prefers_build(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            build_bin = root / "build" / "src" / "bin"
            clean_bin = root / "build_clean_bison" / "src" / "bin"
            build_bin.mkdir(parents=True)
            clean_bin.mkdir(parents=True)
            (build_bin / "lfortran").write_text("", encoding="utf-8")
            (clean_bin / "lfortran").write_text("", encoding="utf-8")

            got = run_mass.resolve_default_lfortran_bin(root)
            self.assertEqual(got, (build_bin / "lfortran").resolve())

    def test_resolve_default_lfortran_bin_falls_back_to_build_clean_bison(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            clean_bin = root / "build_clean_bison" / "src" / "bin"
            clean_bin.mkdir(parents=True)
            (clean_bin / "lfortran").write_text("", encoding="utf-8")

            got = run_mass.resolve_default_lfortran_bin(root)
            self.assertEqual(got, (clean_bin / "lfortran").resolve())


if __name__ == "__main__":
    unittest.main()
