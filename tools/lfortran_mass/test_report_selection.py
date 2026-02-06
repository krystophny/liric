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


if __name__ == "__main__":
    unittest.main()
