#!/usr/bin/env python3
"""Unit tests for mass harness extrafile compile helpers."""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

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

    def test_resolve_default_runtime_lib_uses_bin_adjacent_runtime(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "build" / "src" / "bin"
            runtime_dir = root / "build" / "src" / "runtime"
            bin_dir.mkdir(parents=True)
            runtime_dir.mkdir(parents=True)
            lfortran_bin = bin_dir / "lfortran"
            runtime_lib = runtime_dir / "liblfortran_runtime.so"
            lfortran_bin.write_text("", encoding="utf-8")
            runtime_lib.write_text("", encoding="utf-8")

            with patch.dict("os.environ", {}, clear=False):
                got = run_mass.resolve_default_runtime_lib(root, lfortran_bin)
            self.assertEqual(got, runtime_lib.resolve())

    def test_resolve_default_runtime_lib_prefers_env_dir(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "build" / "src" / "bin"
            runtime_dir = root / "custom_rt"
            bin_dir.mkdir(parents=True)
            runtime_dir.mkdir(parents=True)
            lfortran_bin = bin_dir / "lfortran"
            runtime_lib = runtime_dir / "liblfortran_runtime.so"
            lfortran_bin.write_text("", encoding="utf-8")
            runtime_lib.write_text("", encoding="utf-8")

            with patch.dict("os.environ", {"LFORTRAN_RUNTIME_LIBRARY_DIR": str(runtime_dir)}, clear=False):
                got = run_mass.resolve_default_runtime_lib(root, lfortran_bin)
            self.assertEqual(got, runtime_lib.resolve())

    def test_choose_entrypoint_non_run_prefers_callable_signature(self) -> None:
        ir_text = (
            "define i32 @main(i32 %argc, i8** %argv) {\n"
            "entry:\n"
            "  ret i32 0\n"
            "}\n"
            "define i32 @helper() {\n"
            "entry:\n"
            "  ret i32 7\n"
            "}\n"
        )
        got = run_mass.choose_entrypoint(ir_text, run_requested=False)
        self.assertEqual(got, ("helper", "i32"))

    def test_choose_entrypoint_run_requested_keeps_main_preference(self) -> None:
        ir_text = (
            "define i32 @main(i32 %argc, i8** %argv) {\n"
            "entry:\n"
            "  ret i32 0\n"
            "}\n"
            "define i32 @helper() {\n"
            "entry:\n"
            "  ret i32 7\n"
            "}\n"
        )
        got = run_mass.choose_entrypoint(ir_text, run_requested=True)
        self.assertEqual(got, ("main", "unsupported"))


if __name__ == "__main__":
    unittest.main()
