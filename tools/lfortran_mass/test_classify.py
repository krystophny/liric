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

    def test_taxonomy_unresolved_symbol_maps_to_jit_link_runtime_api(self) -> None:
        row = {
            "classification": classify.UNSUPPORTED_ABI,
            "stage": "jit",
            "reason": "unresolved symbol: _lfortran_pow_i4",
        }
        node = classify.taxonomy_node(row)
        self.assertEqual(node["stage"], "jit-link")
        self.assertEqual(node["symptom"], "unresolved-symbol")
        self.assertEqual(node["feature_family"], "runtime-api")

    def test_taxonomy_mismatch_maps_to_output_format_symptom(self) -> None:
        row = {
            "classification": classify.MISMATCH,
            "stage": "differential",
            "differential_ok": True,
            "differential_match": False,
            "differential_rc_match": True,
            "differential_stdout_match": False,
            "differential_stderr_match": True,
        }
        node = classify.taxonomy_node(row)
        self.assertEqual(node["stage"], "output-format")
        self.assertEqual(node["symptom"], "wrong-stdout")
        self.assertEqual(node["feature_family"], "general")

    def test_taxonomy_counts_support_class_filter(self) -> None:
        rows = [
            {
                "classification": classify.PASS,
                "stage": "differential",
            },
            {
                "classification": classify.UNSUPPORTED_FEATURE,
                "stage": "parse",
                "reason": "unsupported instruction: fneg",
            },
        ]
        counts = classify.taxonomy_counts(
            rows,
            include_classifications={classify.UNSUPPORTED_FEATURE},
        )
        self.assertEqual(len(counts), 1)
        key = next(iter(counts.keys()))
        self.assertEqual(key, "parse|unsupported-feature|general")
        self.assertEqual(counts[key], 1)


if __name__ == "__main__":
    unittest.main()
