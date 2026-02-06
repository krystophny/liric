#!/usr/bin/env python3
"""Reporting utilities for LFortran mass test runs."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

from . import classify


def load_jsonl(path: Path) -> List[Dict[str, Any]]:
    if not path.exists():
        return []
    rows: List[Dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def write_jsonl(path: Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, sort_keys=True, ensure_ascii=True))
            f.write("\n")


def summarize(
    manifest_total: int,
    processed: List[Dict[str, Any]],
    baseline: List[Dict[str, Any]],
    selection_rows: List[Dict[str, Any]],
) -> Dict[str, Any]:
    counts = classify.counts(processed)
    mismatch_count = counts.get(classify.MISMATCH, 0)
    corpus_counts: Dict[str, int] = {}
    for row in processed:
        corpus = str(row.get("corpus", "unknown"))
        corpus_counts[corpus] = corpus_counts.get(corpus, 0) + 1

    selected_by_corpus: Dict[str, int] = {}
    skipped_by_corpus: Dict[str, int] = {}
    skipped_reason_counts: Dict[str, int] = {}
    skipped_by_corpus_reason: Dict[str, int] = {}
    for row in selection_rows:
        corpus = str(row.get("corpus", "unknown"))
        decision = str(row.get("decision", "unknown"))
        reason = str(row.get("reason", "unknown"))
        if decision == "selected":
            selected_by_corpus[corpus] = selected_by_corpus.get(corpus, 0) + 1
            continue
        skipped_by_corpus[corpus] = skipped_by_corpus.get(corpus, 0) + 1
        skipped_reason_counts[reason] = skipped_reason_counts.get(reason, 0) + 1
        key = f"{corpus}:{reason}"
        skipped_by_corpus_reason[key] = skipped_by_corpus_reason.get(key, 0) + 1

    emit_attempted = sum(1 for row in processed if row.get("emit_attempted"))
    emit_ok = sum(1 for row in processed if row.get("emit_ok"))
    parse_attempted = sum(1 for row in processed if row.get("parse_attempted"))
    parse_ok = sum(1 for row in processed if row.get("parse_ok"))
    jit_attempted = sum(1 for row in processed if row.get("jit_attempted"))
    jit_ok = sum(1 for row in processed if row.get("jit_ok"))
    diff_attempted = sum(1 for row in processed if row.get("differential_attempted"))
    diff_ok = sum(1 for row in processed if row.get("differential_ok"))
    diff_match = sum(1 for row in processed if row.get("differential_match") is True)

    unsupported_histogram = {
        classify.UNSUPPORTED_FEATURE: counts.get(classify.UNSUPPORTED_FEATURE, 0),
        classify.UNSUPPORTED_ABI: counts.get(classify.UNSUPPORTED_ABI, 0),
    }

    supported_total = sum(
        1 for row in processed if classify.is_supported_classification(str(row.get("classification")))
    )
    supported_pass = sum(
        1
        for row in processed
        if str(row.get("classification")) == classify.PASS
        and classify.is_supported_classification(str(row.get("classification")))
    )

    baseline_map = {x.get("case_id"): x.get("classification") for x in baseline}
    new_supported_regressions = 0
    regressed_cases: List[str] = []

    for row in processed:
        case_id = row.get("case_id")
        prev = baseline_map.get(case_id)
        now = row.get("classification")
        if prev == classify.PASS and now != classify.PASS:
            if classify.is_supported_classification(str(now)):
                new_supported_regressions += 1
                regressed_cases.append(str(case_id))

    summary = {
        "manifest_total": manifest_total,
        "processed_total": len(processed),
        "emit_attempted": emit_attempted,
        "emit_ok": emit_ok,
        "parse_attempted": parse_attempted,
        "parse_ok": parse_ok,
        "jit_attempted": jit_attempted,
        "jit_ok": jit_ok,
        "differential_attempted": diff_attempted,
        "differential_ok": diff_ok,
        "differential_match": diff_match,
        "classification_counts": counts,
        "corpus_counts": corpus_counts,
        "selected_by_corpus": selected_by_corpus,
        "skipped_by_corpus": skipped_by_corpus,
        "skipped_reason_counts": skipped_reason_counts,
        "skipped_by_corpus_reason": skipped_by_corpus_reason,
        "unsupported_histogram": unsupported_histogram,
        "supported_total": supported_total,
        "supported_pass": supported_pass,
        "mismatch_count": mismatch_count,
        "new_supported_regressions": new_supported_regressions,
        "regressed_case_ids": regressed_cases,
        "gate_fail": mismatch_count > 0 or new_supported_regressions > 0,
    }
    return summary


def write_summary_md(path: Path, summary: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = []
    lines.append("# LFortran Mass Testing Summary")
    lines.append("")
    lines.append(f"- Total selected tests: {summary['manifest_total']}")
    lines.append(f"- Processed tests: {summary['processed_total']}")
    lines.append(f"- LLVM emission attempted: {summary['emit_attempted']}")
    lines.append(f"- LLVM emission succeeded: {summary['emit_ok']}")
    lines.append(f"- Liric parse attempted: {summary['parse_attempted']}")
    lines.append(f"- Liric parse passed: {summary['parse_ok']}")
    lines.append(f"- Liric JIT attempted: {summary['jit_attempted']}")
    lines.append(f"- Liric JIT passed: {summary['jit_ok']}")
    lines.append(f"- Differential attempted: {summary['differential_attempted']}")
    lines.append(f"- Differential completed: {summary['differential_ok']}")
    lines.append(f"- Differential exact matches: {summary['differential_match']}")
    lines.append(f"- Supported processed: {summary['supported_total']}")
    lines.append(f"- Supported passed: {summary['supported_pass']}")
    lines.append(f"- Mismatches: {summary['mismatch_count']}")
    lines.append(
        f"- New supported regressions: {summary['new_supported_regressions']}"
    )
    lines.append(f"- Gate fail: {summary['gate_fail']}")
    lines.append("")
    lines.append("## Corpus Counts")
    lines.append("")
    corpus_counts: Dict[str, int] = summary.get("corpus_counts", {})
    for key in sorted(corpus_counts.keys()):
        lines.append(f"- {key}: {corpus_counts[key]}")

    lines.append("")
    lines.append("## Selection Summary")
    lines.append("")
    selected_by_corpus: Dict[str, int] = summary.get("selected_by_corpus", {})
    skipped_by_corpus: Dict[str, int] = summary.get("skipped_by_corpus", {})
    for key in sorted(set(selected_by_corpus.keys()) | set(skipped_by_corpus.keys())):
        lines.append(
            f"- {key}: selected={selected_by_corpus.get(key, 0)}, "
            f"skipped={skipped_by_corpus.get(key, 0)}"
        )

    lines.append("")
    lines.append("## Skip Reasons")
    lines.append("")
    skip_reasons: Dict[str, int] = summary.get("skipped_reason_counts", {})
    for key in sorted(skip_reasons.keys()):
        lines.append(f"- {key}: {skip_reasons[key]}")

    lines.append("")
    lines.append("## Skip Reasons By Corpus")
    lines.append("")
    skip_by_corpus_reason: Dict[str, int] = summary.get("skipped_by_corpus_reason", {})
    for key in sorted(skip_by_corpus_reason.keys()):
        lines.append(f"- {key}: {skip_by_corpus_reason[key]}")

    lines.append("")
    lines.append("## Classification Counts")
    lines.append("")

    counts: Dict[str, int] = summary.get("classification_counts", {})
    for key in sorted(counts.keys()):
        lines.append(f"- {key}: {counts[key]}")

    lines.append("")
    lines.append("## Unsupported Histogram")
    lines.append("")
    histogram: Dict[str, int] = summary.get("unsupported_histogram", {})
    for key in sorted(histogram.keys()):
        lines.append(f"- {key}: {histogram[key]}")

    if summary.get("regressed_case_ids"):
        lines.append("")
        lines.append("## New Supported Regressions")
        lines.append("")
        for case_id in summary["regressed_case_ids"]:
            lines.append(f"- {case_id}")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_failures_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "case_id",
                "filename",
                "source_path",
                "classification",
                "stage",
                "reason",
            ]
        )
        for row in rows:
            cls = row.get("classification", classify.INFRA_FAIL)
            if cls == classify.PASS:
                continue
            writer.writerow(
                [
                    row.get("case_id", ""),
                    row.get("filename", ""),
                    row.get("source_path", ""),
                    cls,
                    row.get("stage", ""),
                    row.get("reason", ""),
                ]
            )


def gate_result(summary: Dict[str, Any]) -> Tuple[bool, str]:
    if summary.get("gate_fail", False):
        return False, "mismatch or new supported regressions detected"
    return True, "no mismatches and no new supported regressions"
