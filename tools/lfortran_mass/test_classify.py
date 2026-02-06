#!/usr/bin/env python3
"""Unit tests for mass harness classification helpers."""

from __future__ import annotations

import unittest

from tools.lfortran_mass import classify


class ClassifyTests(unittest.TestCase):
    def test_jit_unresolved_symbol_is_unsupported_abi(self) -> None:
        got = classify.classify_jit_failure("unresolved symbol: llvm.powi.f32\n", "")
        self.assertEqual(got, classify.UNSUPPORTED_ABI)

    def test_jit_function_not_found_is_unsupported_abi(self) -> None:
        got = classify.classify_jit_failure("function 'main' not found\n", "")
        self.assertEqual(got, classify.UNSUPPORTED_ABI)

    def test_jit_unsupported_signature_is_unsupported_abi(self) -> None:
        got = classify.classify_jit_failure("unsupported signature: i32(i32)\n", "")
        self.assertEqual(got, classify.UNSUPPORTED_ABI)

    def test_jit_generic_failure_is_liric_jit_fail(self) -> None:
        ir = "declare void @_lfortran_runtime_error(ptr)\n"
        got = classify.classify_jit_failure("segmentation fault\n", ir)
        self.assertEqual(got, classify.LIRIC_JIT_FAIL)

    def test_jit_explicit_unsupported_message_is_unsupported_feature(self) -> None:
        got = classify.classify_jit_failure("unsupported opcode in backend\n", "")
        self.assertEqual(got, classify.UNSUPPORTED_FEATURE)

    def test_jit_ir_feature_pattern_is_unsupported_feature(self) -> None:
        ir = "%1 = fcmp oeq float %a, %b\n"
        got = classify.classify_jit_failure("jit failed\n", ir)
        self.assertEqual(got, classify.UNSUPPORTED_FEATURE)


if __name__ == "__main__":
    unittest.main()
