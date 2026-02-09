#!/usr/bin/env python3
"""Unit tests for mass harness extrafile compile helpers."""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from tools.lfortran_mass import run_mass


class RunMassHelperTests(unittest.TestCase):
    def test_probe_run_command_ignore_retcode_flag(self) -> None:
        cfg = run_mass.RunnerConfig(
            lfortran_bin=Path("/tmp/lfortran"),
            liric_bin=Path("/tmp/liric"),
            probe_runner=Path("/tmp/liric_probe_runner"),
            cache_dir=Path("/tmp/cache"),
            timeout_emit=1,
            timeout_parse=1,
            timeout_jit=1,
            timeout_run=1,
            force=False,
            runtime_libs=("/tmp/libA.so", "/tmp/libB.so"),
        )
        got = run_mass.probe_run_command(
            cfg,
            Path("/tmp/test.ll"),
            "main",
            "i32_argc_argv",
            ignore_retcode=True,
        )
        self.assertIn("--ignore-retcode", got)
        self.assertEqual(got[-1], "/tmp/test.ll")

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

    def test_entry_needs_openmp_runtime_from_option(self) -> None:
        entry = SimpleNamespace(options="--openmp -O0", labels=[])
        self.assertTrue(run_mass.entry_needs_openmp_runtime(entry))

    def test_entry_needs_openmp_runtime_from_label(self) -> None:
        entry = SimpleNamespace(options="", labels=["llvm", "llvm_omp"])
        self.assertTrue(run_mass.entry_needs_openmp_runtime(entry))

    def test_resolve_default_openmp_lib_prefers_explicit_env_file(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "build" / "src" / "bin"
            omp_dir = root / "custom_omp"
            bin_dir.mkdir(parents=True)
            omp_dir.mkdir(parents=True)
            lfortran_bin = bin_dir / "lfortran"
            openmp_lib = omp_dir / "libgomp_custom.so"
            lfortran_bin.write_text("", encoding="utf-8")
            openmp_lib.write_text("", encoding="utf-8")

            env = {"LFORTRAN_OPENMP_LIBRARY": str(openmp_lib)}
            with patch.dict("os.environ", env, clear=False):
                with patch.object(run_mass, "DEFAULT_OPENMP_LIB_DIRS", ()):
                    got = run_mass.resolve_default_openmp_lib(root, lfortran_bin)
            self.assertEqual(got, openmp_lib.resolve())

    def test_resolve_default_openmp_lib_prefers_env_dir(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "build" / "src" / "bin"
            omp_dir = root / "omp_runtime"
            bin_dir.mkdir(parents=True)
            omp_dir.mkdir(parents=True)
            lfortran_bin = bin_dir / "lfortran"
            openmp_lib = omp_dir / "libgomp.so"
            lfortran_bin.write_text("", encoding="utf-8")
            openmp_lib.write_text("", encoding="utf-8")

            env = {"LFORTRAN_OPENMP_LIBRARY_DIR": str(omp_dir)}
            with patch.dict("os.environ", env, clear=False):
                with patch.object(run_mass, "DEFAULT_OPENMP_LIB_DIRS", ()):
                    got = run_mass.resolve_default_openmp_lib(root, lfortran_bin)
            self.assertEqual(got, openmp_lib.resolve())

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
        self.assertEqual(got, ("main", "i32_argc_argv"))

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
        self.assertEqual(got, ("main", "i32_argc_argv"))

    def test_signature_kind_accepts_main_i8pp(self) -> None:
        got = run_mass.signature_kind("i32", "i32 %argc, i8** %argv")
        self.assertEqual(got, "i32_argc_argv")

    def test_signature_kind_accepts_main_ptr_opaque(self) -> None:
        got = run_mass.signature_kind("i32", "i32 noundef %argc, ptr noundef %argv")
        self.assertEqual(got, "i32_argc_argv")

    def test_load_name_list_dedupes_and_skips_comments(self) -> None:
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "compat.txt"
            path.write_text(
                "\n".join(["", "# comment", "foo", "bar", "foo", "  baz  "]) + "\n",
                encoding="utf-8",
            )
            got = run_mass.load_name_list(path)
            self.assertEqual(got, ["foo", "bar", "baz"])

    def test_select_entry_for_compat_api(self) -> None:
        integration = SimpleNamespace(corpus="integration_cmake", name="foo")
        tests_toml = SimpleNamespace(corpus="tests_toml", name="")

        include, reason = run_mass.select_entry_for_compat_api(integration, {"foo"})
        self.assertTrue(include)
        self.assertEqual(reason, "included")

        include, reason = run_mass.select_entry_for_compat_api(integration, {"bar"})
        self.assertFalse(include)
        self.assertEqual(reason, "not_in_compat_api_list")

        include, reason = run_mass.select_entry_for_compat_api(tests_toml, {"foo"})
        self.assertFalse(include)
        self.assertEqual(reason, "compat_api_list_mode")


if __name__ == "__main__":
    unittest.main()
