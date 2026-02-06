#!/usr/bin/env python3
"""Classification helpers for LFortran->Liric mass testing."""

from __future__ import annotations

import re
from typing import Dict

PASS = "pass"
MISMATCH = "mismatch"
LFORTRAN_EMIT_FAIL = "lfortran_emit_fail"
LIRIC_PARSE_FAIL = "liric_parse_fail"
LIRIC_JIT_FAIL = "liric_jit_fail"
UNSUPPORTED_FEATURE = "unsupported_feature"
UNSUPPORTED_ABI = "unsupported_abi"
INFRA_FAIL = "infra_fail"

UNSUPPORTED_FEATURE_PATTERNS = [
    r"\bfadd\b",
    r"\bfsub\b",
    r"\bfmul\b",
    r"\bfdiv\b",
    r"\bfloat\b",
    r"\bdouble\b",
    r"<\s*\d+\s*x\s*",
    r"\bx86_amx\b",
    r"\btarget-features\b",
]

UNSUPPORTED_ABI_PATTERNS = [
    r"@_lfortran_",
    r"@_lpython_",
    r"@_lcompilers_",
    r"@malloc",
    r"@free",
]


def detect_unsupported_feature(ir_text: str) -> bool:
    for pattern in UNSUPPORTED_FEATURE_PATTERNS:
        if re.search(pattern, ir_text):
            return True
    return False


def detect_unsupported_abi(ir_text: str) -> bool:
    for pattern in UNSUPPORTED_ABI_PATTERNS:
        if re.search(pattern, ir_text):
            return True
    return False


def classify_parse_failure(stderr: str, ir_text: str) -> str:
    msg = stderr.lower()
    if detect_unsupported_feature(ir_text):
        return UNSUPPORTED_FEATURE
    if "expected type" in msg or "unexpected token" in msg:
        return UNSUPPORTED_FEATURE
    return LIRIC_PARSE_FAIL


def classify_jit_failure(stderr: str, ir_text: str) -> str:
    msg = stderr.lower()
    if detect_unsupported_abi(ir_text):
        return UNSUPPORTED_ABI
    if "function" in msg and "not found" in msg:
        return UNSUPPORTED_ABI
    if "unsupported" in msg:
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
