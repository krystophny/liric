#!/usr/bin/env python3
"""Classification helpers for LFortran->Liric mass testing."""

from __future__ import annotations

import re
from typing import Dict, Iterable, Mapping, Optional, Set

PASS = "pass"
MISMATCH = "mismatch"
LFORTRAN_EMIT_FAIL = "lfortran_emit_fail"
LIRIC_PARSE_FAIL = "liric_parse_fail"
LIRIC_JIT_FAIL = "liric_jit_fail"
UNSUPPORTED_FEATURE = "unsupported_feature"
UNSUPPORTED_ABI = "unsupported_abi"
INFRA_FAIL = "infra_fail"

TAXON_STAGE_PARSE = "parse"
TAXON_STAGE_CODEGEN = "codegen"
TAXON_STAGE_JIT_LINK = "jit-link"
TAXON_STAGE_RUNTIME = "runtime"
TAXON_STAGE_OUTPUT_FORMAT = "output-format"

TAXON_SYMPTOM_SEGFAULT = "segfault"
TAXON_SYMPTOM_UNRESOLVED_SYMBOL = "unresolved-symbol"
TAXON_SYMPTOM_WRONG_STDOUT = "wrong-stdout"
TAXON_SYMPTOM_WRONG_STDERR = "wrong-stderr"
TAXON_SYMPTOM_WRONG_STDOUT_STDERR = "wrong-stdout+stderr"
TAXON_SYMPTOM_RC_MISMATCH = "rc-mismatch"
TAXON_SYMPTOM_UNSUPPORTED_FEATURE = "unsupported-feature"
TAXON_SYMPTOM_UNSUPPORTED_ABI = "unsupported-abi"
TAXON_SYMPTOM_COMPILER_ERROR = "compiler-error"
TAXON_SYMPTOM_INFRA_FAIL = "infra-fail"
TAXON_SYMPTOM_UNKNOWN = "unknown"

TAXON_FAMILY_INTRINSICS = "intrinsics"
TAXON_FAMILY_COMPLEX = "complex"
TAXON_FAMILY_OPENMP = "openmp"
TAXON_FAMILY_MULTI_FILE = "multi-file"
TAXON_FAMILY_RUNTIME_API = "runtime-api"
TAXON_FAMILY_GENERAL = "general"

UNSUPPORTED_FEATURE_PATTERNS = [
    r"\bfcmp\b",
    r"\bsitofp\b",
    r"\bfptosi\b",
    r"\bfpext\b",
    r"\bfptrunc\b",
    r"\bfneg\b",
    r"<\s*\d+\s*x\s*",
    r"\bx86_amx\b",
    r"\btarget-features\b",
]

UNSUPPORTED_ABI_JIT_PATTERNS = [
    r"\bunresolved symbol:\s*[^\s]+",
    r"\bundefined symbol\b",
    r"\bfunction\s+'[^']+'\s+not found\b",
    r'\bfunction\s+"[^"]+"\s+not found\b',
    r"\bfunction\s+[^\s]+\s+not found\b",
    r"\bunsupported signature\b",
    r"\bunsupported entry signature\b",
]

UNSUPPORTED_FEATURE_JIT_PATTERNS = [
    r"\bunsupported\s+(instruction|opcode|operation|feature|type|intrinsic|wasm)\b",
    r"\bnot implemented\b",
]


def detect_unsupported_feature(ir_text: str) -> bool:
    for pattern in UNSUPPORTED_FEATURE_PATTERNS:
        if re.search(pattern, ir_text):
            return True
    return False


def detect_unsupported_abi_jit(stderr: str) -> bool:
    for pattern in UNSUPPORTED_ABI_JIT_PATTERNS:
        if re.search(pattern, stderr):
            return True
    return False


def classify_parse_failure(stderr: str, ir_text: str) -> str:
    msg = stderr.lower()
    if detect_unsupported_feature(ir_text):
        return UNSUPPORTED_FEATURE
    if (
        "expected type" in msg
        or "unexpected token" in msg
        or "expected operand" in msg
        or "unknown instruction" in msg
    ):
        return UNSUPPORTED_FEATURE
    return LIRIC_PARSE_FAIL


def classify_jit_failure(stderr: str, ir_text: str) -> str:
    msg = stderr.lower()
    if detect_unsupported_abi_jit(msg):
        return UNSUPPORTED_ABI
    for pattern in UNSUPPORTED_FEATURE_JIT_PATTERNS:
        if re.search(pattern, msg):
            return UNSUPPORTED_FEATURE
    if detect_unsupported_feature(ir_text):
        return UNSUPPORTED_FEATURE
    return LIRIC_JIT_FAIL


def is_supported_classification(classification: str) -> bool:
    return classification not in {UNSUPPORTED_FEATURE, UNSUPPORTED_ABI}


def counts(rows: list[Dict[str, str]]) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        key = row.get("classification", INFRA_FAIL)
        out[key] = out.get(key, 0) + 1
    return out


def _row_text(row: Mapping[str, object]) -> str:
    parts = [
        str(row.get("reason", "")),
        str(row.get("stage", "")),
        str(row.get("source_path", "")),
        str(row.get("filename", "")),
        str(row.get("case_id", "")),
        str(row.get("classification", "")),
    ]
    return " ".join(parts).lower()


def _taxonomy_stage(classification: str, stage: str, text: str) -> str:
    if classification == MISMATCH or stage == "differential":
        return TAXON_STAGE_OUTPUT_FORMAT
    if stage == "parse":
        return TAXON_STAGE_PARSE
    if stage in {"emit", "emit_prep"}:
        return TAXON_STAGE_CODEGEN
    if stage in {"entrypoint", "differential_prepare"}:
        return TAXON_STAGE_JIT_LINK
    if stage == "jit":
        if "unresolved symbol" in text or "undefined symbol" in text:
            return TAXON_STAGE_JIT_LINK
        if " not found" in text:
            return TAXON_STAGE_JIT_LINK
        return TAXON_STAGE_RUNTIME
    return TAXON_STAGE_RUNTIME


def _taxonomy_mismatch_symptom(row: Mapping[str, object]) -> str:
    rc_match = row.get("differential_rc_match")
    stdout_match = row.get("differential_stdout_match")
    stderr_match = row.get("differential_stderr_match")
    if rc_match is False:
        return TAXON_SYMPTOM_RC_MISMATCH
    if stdout_match is False and stderr_match is False:
        return TAXON_SYMPTOM_WRONG_STDOUT_STDERR
    if stdout_match is False:
        return TAXON_SYMPTOM_WRONG_STDOUT
    if stderr_match is False:
        return TAXON_SYMPTOM_WRONG_STDERR
    return TAXON_SYMPTOM_UNKNOWN


def _taxonomy_symptom(classification: str, row: Mapping[str, object], text: str) -> str:
    if classification == MISMATCH:
        return _taxonomy_mismatch_symptom(row)
    if "segmentation fault" in text or "sigsegv" in text:
        return TAXON_SYMPTOM_SEGFAULT
    if "unresolved symbol" in text or "undefined symbol" in text:
        return TAXON_SYMPTOM_UNRESOLVED_SYMBOL
    if " not found" in text and "function" in text:
        return TAXON_SYMPTOM_UNRESOLVED_SYMBOL
    if classification == UNSUPPORTED_FEATURE:
        return TAXON_SYMPTOM_UNSUPPORTED_FEATURE
    if classification == UNSUPPORTED_ABI:
        return TAXON_SYMPTOM_UNSUPPORTED_ABI
    if classification in {LIRIC_PARSE_FAIL, LFORTRAN_EMIT_FAIL, LIRIC_JIT_FAIL}:
        return TAXON_SYMPTOM_COMPILER_ERROR
    if classification == INFRA_FAIL:
        return TAXON_SYMPTOM_INFRA_FAIL
    return TAXON_SYMPTOM_UNKNOWN


def _taxonomy_feature_family(text: str) -> str:
    if "openmp" in text or "libgomp" in text or "omp_" in text:
        return TAXON_FAMILY_OPENMP
    if "complex" in text:
        return TAXON_FAMILY_COMPLEX
    if "llvm." in text or "intrinsic" in text:
        return TAXON_FAMILY_INTRINSICS
    if "extrafile" in text or "multi-file" in text:
        return TAXON_FAMILY_MULTI_FILE
    if "lfortran_runtime" in text or "_lfortran_" in text:
        return TAXON_FAMILY_RUNTIME_API
    if "entry signature" in text or "unsupported signature" in text:
        return TAXON_FAMILY_RUNTIME_API
    return TAXON_FAMILY_GENERAL


def taxonomy_node(row: Mapping[str, object]) -> Dict[str, str]:
    classification = str(row.get("classification", INFRA_FAIL))
    stage = str(row.get("stage", "")).lower()
    text = _row_text(row)
    out_stage = _taxonomy_stage(classification, stage, text)
    out_symptom = _taxonomy_symptom(classification, row, text)
    out_feature_family = _taxonomy_feature_family(text)
    return {
        "stage": out_stage,
        "symptom": out_symptom,
        "feature_family": out_feature_family,
        "node": f"{out_stage}|{out_symptom}|{out_feature_family}",
    }


def taxonomy_counts(
    rows: Iterable[Mapping[str, object]],
    include_classifications: Optional[Set[str]] = None,
    exclude_pass: bool = False,
) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        classification = str(row.get("classification", INFRA_FAIL))
        if exclude_pass and classification == PASS:
            continue
        if include_classifications is not None and classification not in include_classifications:
            continue
        node = taxonomy_node(row)["node"]
        out[node] = out.get(node, 0) + 1
    return out
