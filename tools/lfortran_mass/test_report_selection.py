#!/usr/bin/env python3
"""Unit tests for selection/skip accounting in report summary."""

from __future__ import annotations

import unittest

from tools.lfortran_mass import report


class ReportSelectionSummaryTests(unittest.TestCase):
    def test_selection_skip_histograms(self) -> None:
        processed = [
            {"case_id": "a", "classification": "pass", "corpus": "tests_toml"},
            {"case_id": "b", "classification": "unsupported_abi", "corpus": "integration_cmake"},
        ]
        selection_rows = [
            {"corpus": "tests_toml", "decision": "selected", "reason": "included"},
            {"corpus": "integration_cmake", "decision": "selected", "reason": "included"},
            {"corpus": "tests_toml", "decision": "skipped", "reason": "not_llvm_intended"},
            {"corpus": "tests_toml", "decision": "skipped", "reason": "expected_failure"},
            {"corpus": "integration_cmake", "decision": "skipped", "reason": "duplicate_case"},
        ]

        summary = report.summarize(
            manifest_total=2,
            processed=processed,
            baseline=[],
            selection_rows=selection_rows,
        )

        self.assertEqual(summary["selected_by_corpus"]["tests_toml"], 1)
        self.assertEqual(summary["selected_by_corpus"]["integration_cmake"], 1)
        self.assertEqual(summary["skipped_by_corpus"]["tests_toml"], 2)
        self.assertEqual(summary["skipped_by_corpus"]["integration_cmake"], 1)
        self.assertEqual(summary["skipped_reason_counts"]["not_llvm_intended"], 1)
        self.assertEqual(summary["skipped_reason_counts"]["expected_failure"], 1)
        self.assertEqual(summary["skipped_reason_counts"]["duplicate_case"], 1)
        self.assertEqual(summary["skipped_by_corpus_reason"]["tests_toml:expected_failure"], 1)

    def test_differential_mismatch_buckets(self) -> None:
        processed = [
            {
                "case_id": "a",
                "classification": "mismatch",
                "corpus": "integration_cmake",
                "differential_ok": True,
                "differential_match": False,
                "differential_rc_match": False,
                "differential_stdout_match": True,
                "differential_stderr_match": True,
            },
            {
                "case_id": "b",
                "classification": "mismatch",
                "corpus": "integration_cmake",
                "differential_ok": True,
                "differential_match": False,
                "differential_rc_match": True,
                "differential_stdout_match": False,
                "differential_stderr_match": False,
            },
            {
                "case_id": "c",
                "classification": "mismatch",
                "corpus": "integration_cmake",
                "differential_ok": False,
                "differential_match": False,
            },
        ]
        summary = report.summarize(
            manifest_total=3,
            processed=processed,
            baseline=[],
            selection_rows=[],
        )

        self.assertEqual(summary["differential_mismatch_total"], 3)
        self.assertEqual(summary["differential_mismatch_buckets"]["rc"], 1)
        self.assertEqual(summary["differential_mismatch_buckets"]["stdout_stderr"], 1)
        self.assertEqual(summary["differential_mismatch_buckets"]["unknown"], 1)

    def test_taxonomy_histograms_are_reported(self) -> None:
        processed = [
            {
                "case_id": "mismatch_stdout",
                "classification": "mismatch",
                "stage": "differential",
                "differential_ok": True,
                "differential_match": False,
                "differential_rc_match": True,
                "differential_stdout_match": False,
                "differential_stderr_match": True,
            },
            {
                "case_id": "unsupported_runtime",
                "classification": "unsupported_abi",
                "stage": "jit",
                "reason": "unresolved symbol: _lfortran_abort",
            },
            {
                "case_id": "pass_case",
                "classification": "pass",
                "stage": "differential",
            },
        ]
        summary = report.summarize(
            manifest_total=3,
            processed=processed,
            baseline=[],
            selection_rows=[],
        )

        self.assertEqual(
            summary["taxonomy_counts"]["output-format|wrong-stdout|general"], 1
        )
        self.assertEqual(
            summary["taxonomy_counts"]["jit-link|unresolved-symbol|runtime-api"], 1
        )
        self.assertEqual(len(summary["taxonomy_counts"]), 2)
        self.assertEqual(
            summary["mismatch_taxonomy_counts"]["output-format|wrong-stdout|general"], 1
        )
        self.assertEqual(
            summary["unsupported_taxonomy_counts"][
                "jit-link|unresolved-symbol|runtime-api"
            ],
            1,
        )

    def test_differential_parity_gap_is_gated(self) -> None:
        processed = [
            {
                "case_id": "ok",
                "classification": "pass",
                "run_requested": True,
                "jit_ok": True,
                "differential_attempted": True,
            },
            {
                "case_id": "missing",
                "classification": "pass",
                "run_requested": True,
                "jit_ok": True,
                "differential_attempted": False,
            },
            {
                "case_id": "not_executable",
                "classification": "unsupported_feature",
                "run_requested": True,
                "jit_ok": False,
                "differential_attempted": False,
            },
        ]
        summary = report.summarize(
            manifest_total=3,
            processed=processed,
            baseline=[],
            selection_rows=[],
        )

        self.assertEqual(summary["runnable_selected"], 3)
        self.assertEqual(summary["runnable_selected_executable"], 2)
        self.assertEqual(summary["differential_attempted_executable"], 1)
        self.assertEqual(summary["differential_missing_attempts"], 1)
        self.assertEqual(summary["differential_missing_case_ids"], ["missing"])
        self.assertFalse(summary["differential_parity_ok"])
        self.assertTrue(summary["gate_fail"])

        ok, message = report.gate_result(summary)
        self.assertFalse(ok)
        self.assertEqual(message, "differential coverage regression detected")


if __name__ == "__main__":
    unittest.main()
