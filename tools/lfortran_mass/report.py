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


def write_summary_json(path: Path, summary: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(summary, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def summarize(
    manifest_total: int,
    processed: List[Dict[str, Any]],
    baseline: List[Dict[str, Any]],
    selection_rows: List[Dict[str, Any]],
) -> Dict[str, Any]:
    counts = classify.counts(processed)
    taxonomy_counts = classify.taxonomy_counts(processed, exclude_pass=True)
    mismatch_taxonomy_counts = classify.taxonomy_counts(
        processed, include_classifications={classify.MISMATCH}
    )
    unsupported_taxonomy_counts = classify.taxonomy_counts(
        processed,
        include_classifications={classify.UNSUPPORTED_FEATURE, classify.UNSUPPORTED_ABI},
    )
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
    runnable_selected = sum(1 for row in processed if row.get("run_requested"))
    runnable_selected_executable = sum(
        1 for row in processed if row.get("run_requested") and row.get("jit_ok")
    )
    diff_attempted = sum(1 for row in processed if row.get("differential_attempted"))
    diff_attempted_executable = sum(
        1
        for row in processed
        if row.get("run_requested") and row.get("jit_ok") and row.get("differential_attempted")
    )
    diff_ok = sum(1 for row in processed if row.get("differential_ok"))
    diff_match = sum(1 for row in processed if row.get("differential_match") is True)
    diff_missing_case_ids: List[str] = []
    for row in processed:
        if not row.get("run_requested"):
            continue
        if not row.get("jit_ok"):
            continue
        if row.get("differential_attempted"):
            continue
        diff_missing_case_ids.append(str(row.get("case_id", "")))
    diff_missing_attempts = len(diff_missing_case_ids)
    differential_parity_ok = diff_missing_attempts == 0 and (
        diff_attempted_executable == runnable_selected_executable
    )
    diff_mismatch_buckets: Dict[str, int] = {}
    for row in processed:
        if row.get("differential_match") is not False:
            continue
        if not row.get("differential_ok"):
            key = "unknown"
        else:
            parts: List[str] = []
            if not row.get("differential_rc_match"):
                parts.append("rc")
            if not row.get("differential_stdout_match"):
                parts.append("stdout")
            if not row.get("differential_stderr_match"):
                parts.append("stderr")
            key = "_".join(parts) if parts else "unknown"
        diff_mismatch_buckets[key] = diff_mismatch_buckets.get(key, 0) + 1
    diff_mismatch_total = sum(diff_mismatch_buckets.values())

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
        "runnable_selected": runnable_selected,
        "runnable_selected_executable": runnable_selected_executable,
        "differential_attempted": diff_attempted,
        "differential_attempted_executable": diff_attempted_executable,
        "differential_ok": diff_ok,
        "differential_match": diff_match,
        "differential_missing_attempts": diff_missing_attempts,
        "differential_missing_case_ids": diff_missing_case_ids,
        "differential_parity_ok": differential_parity_ok,
        "differential_mismatch_total": diff_mismatch_total,
        "differential_mismatch_buckets": diff_mismatch_buckets,
        "classification_counts": counts,
        "taxonomy_counts": taxonomy_counts,
        "mismatch_taxonomy_counts": mismatch_taxonomy_counts,
        "unsupported_taxonomy_counts": unsupported_taxonomy_counts,
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
        "gate_fail": (
            mismatch_count > 0
            or new_supported_regressions > 0
            or (not differential_parity_ok)
        ),
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
    lines.append(f"- Runnable selected: {summary['runnable_selected']}")
    lines.append(
        f"- Runnable selected executable: {summary['runnable_selected_executable']}"
    )
    lines.append(f"- Differential attempted: {summary['differential_attempted']}")
    lines.append(
        f"- Differential attempted executable: {summary['differential_attempted_executable']}"
    )
    lines.append(f"- Differential completed: {summary['differential_ok']}")
    lines.append(f"- Differential exact matches: {summary['differential_match']}")
    lines.append(f"- Differential mismatches: {summary['differential_mismatch_total']}")
    lines.append(
        f"- Differential missing attempts: {summary['differential_missing_attempts']}"
    )
    lines.append(f"- Differential parity ok: {summary['differential_parity_ok']}")
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
    lines.append("## Taxonomy Counts")
    lines.append("")
    tax_counts: Dict[str, int] = summary.get("taxonomy_counts", {})
    for key in sorted(tax_counts.keys()):
        lines.append(f"- {key}: {tax_counts[key]}")

    lines.append("")
    lines.append("## Taxonomy Counts (Mismatch)")
    lines.append("")
    mismatch_tax: Dict[str, int] = summary.get("mismatch_taxonomy_counts", {})
    for key in sorted(mismatch_tax.keys()):
        lines.append(f"- {key}: {mismatch_tax[key]}")

    lines.append("")
    lines.append("## Taxonomy Counts (Unsupported)")
    lines.append("")
    unsupported_tax: Dict[str, int] = summary.get("unsupported_taxonomy_counts", {})
    for key in sorted(unsupported_tax.keys()):
        lines.append(f"- {key}: {unsupported_tax[key]}")

    lines.append("")
    lines.append("## Differential Mismatch Buckets")
    lines.append("")
    mismatch_buckets: Dict[str, int] = summary.get("differential_mismatch_buckets", {})
    for key in sorted(mismatch_buckets.keys()):
        lines.append(f"- {key}: {mismatch_buckets[key]}")

    missing_case_ids: List[str] = summary.get("differential_missing_case_ids", [])
    if missing_case_ids:
        lines.append("")
        lines.append("## Differential Coverage Gaps")
        lines.append("")
        for case_id in missing_case_ids:
            lines.append(f"- {case_id}")

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
    if not summary.get("differential_parity_ok", True):
        return False, "differential coverage regression detected"
    if summary.get("gate_fail", False):
        return False, "mismatch or new supported regressions detected"
    return True, "no mismatches and no new supported regressions"
