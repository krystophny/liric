#!/usr/bin/env python3
"""Parse LFortran integration_tests/CMakeLists.txt RUN entries."""

from __future__ import annotations

import shlex
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

from .lfortran_tests_toml import file_sha256
from .lfortran_tests_toml import stable_case_id


FLAG_KEYS = {
    "FAIL",
    "NOFAST_TILL_LLVM16",
    "NO_FAST",
    "NO_STD_F23",
    "OLD_CLASSES",
    "NO_LLVM_GOC",
}
ONE_VALUE_KEYS = {"NAME", "FILE", "INCLUDE_PATH", "COPY_TO_BIN"}
MULTI_VALUE_KEYS = {"LABELS", "EXTRAFILES", "EXTRA_ARGS", "GFORTRAN_ARGS"}


@dataclass
class IntegrationRunEntry:
    corpus: str
    index: int
    filename_raw: str
    source_path: Path
    extrafiles_raw: List[str]
    extrafiles: List[Path]
    options: str
    llvm: bool
    run: bool
    run_with_dbg: bool
    asr_implicit_interface_and_typing_with_llvm: bool
    pass_with_llvm: bool
    pass_name: str
    fast: bool
    obj: bool
    bin: bool
    name: str
    labels: List[str]
    expected_fail: bool

    def recipe_kind(self) -> str:
        return "integration_llvm"

    def to_manifest_record(self, lfortran_version: str) -> Dict[str, Any]:
        source_hash = file_sha256(self.source_path)
        case_id = stable_case_id(
            source_path=str(self.source_path),
            source_hash=source_hash,
            lfortran_version=lfortran_version,
            recipe_kind=self.recipe_kind(),
            options=self.options,
            source_tag=f"{self.corpus}:{self.name}",
        )
        return {
            "case_id": case_id,
            "corpus": self.corpus,
            "index": self.index,
            "filename": self.filename_raw,
            "source_path": str(self.source_path),
            "source_hash": source_hash,
            "extrafiles": [str(x) for x in self.extrafiles],
            "options": self.options,
            "recipe_kind": self.recipe_kind(),
            "run": self.run,
            "run_with_dbg": self.run_with_dbg,
            "llvm": self.llvm,
            "pass_with_llvm": self.pass_with_llvm,
            "pass": self.pass_name,
            "fast": self.fast,
            "asr_implicit_interface_and_typing_with_llvm": (
                self.asr_implicit_interface_and_typing_with_llvm
            ),
            "integration_name": self.name,
            "integration_labels": self.labels,
            "expected_fail": self.expected_fail,
        }


def _strip_comments(text: str) -> str:
    out_lines: List[str] = []
    for raw in text.splitlines():
        in_quote = False
        chars: List[str] = []
        for ch in raw:
            if ch == '"':
                in_quote = not in_quote
            if ch == "#" and not in_quote:
                break
            chars.append(ch)
        out_lines.append("".join(chars))
    return "\n".join(out_lines)


def _parse_run_block(raw: str) -> Dict[str, Any]:
    tokens = shlex.split(raw, posix=True)
    data: Dict[str, Any] = {}
    current_multi: Optional[str] = None

    i = 0
    while i < len(tokens):
        tok = tokens[i]

        if tok in FLAG_KEYS:
            data[tok] = True
            current_multi = None
            i += 1
            continue

        if tok in ONE_VALUE_KEYS:
            if i + 1 < len(tokens):
                data[tok] = tokens[i + 1]
                i += 2
                current_multi = None
                continue
            i += 1
            continue

        if tok in MULTI_VALUE_KEYS:
            current_multi = tok
            data.setdefault(tok, [])
            i += 1
            continue

        if current_multi is not None:
            data[current_multi].append(tok)
            i += 1
            continue

        i += 1

    return data


def _resolve_source(integration_dir: Path, file_token: str) -> Path:
    p = Path(file_token)
    if p.suffix:
        return (integration_dir / p).resolve()
    return (integration_dir / f"{file_token}.f90").resolve()


def _join_options(tokens: List[str]) -> str:
    return " ".join(shlex.quote(x) for x in tokens)


def _append_unique(tokens: List[str], new_tokens: List[str]) -> None:
    for token in new_tokens:
        if token not in tokens:
            tokens.append(token)


def _llvm_label_options(labels: List[str]) -> List[str]:
    opts: List[str] = []
    if "llvmImplicit" in labels:
        _append_unique(opts, ["--implicit-typing", "--implicit-interface"])
    if "llvmStackArray" in labels:
        _append_unique(opts, ["--stack-arrays=true"])
    if "llvm_integer_8" in labels:
        _append_unique(opts, ["-fdefault-integer-8"])
    if "llvm_nopragma" in labels:
        _append_unique(opts, ["--ignore-pragma"])
    if "llvm_omp" in labels:
        _append_unique(opts, ["--openmp"])
    if "llvm2" in labels:
        _append_unique(opts, ["--separate-compilation"])
    if "llvm_rtlib" in labels:
        _append_unique(opts, ["--separate-compilation", "--rtlib"])
    return opts


def iter_integration_entries(cmake_path: Path) -> Iterable[IntegrationRunEntry]:
    text = _strip_comments(cmake_path.read_text(encoding="utf-8", errors="replace"))
    integration_dir = cmake_path.parent

    idx = 0
    for match in re.finditer(r"\bRUN\s*\((.*?)\)", text, flags=re.DOTALL):
        parsed = _parse_run_block(match.group(1))
        name = str(parsed.get("NAME", "")).strip()
        if not name:
            continue

        file_token = str(parsed.get("FILE", name)).strip()
        source_path = _resolve_source(integration_dir, file_token)

        labels = [str(x) for x in parsed.get("LABELS", [])]
        extrafiles_raw = [str(x) for x in parsed.get("EXTRAFILES", [])]
        extrafiles = [(integration_dir / x).resolve() for x in extrafiles_raw]

        extra_args = [str(x) for x in parsed.get("EXTRA_ARGS", [])]
        _append_unique(extra_args, _llvm_label_options(labels))
        include_path = str(parsed.get("INCLUDE_PATH", "")).strip()
        if include_path:
            extra_args.extend([f"-I{(integration_dir / include_path).resolve()}"])

        entry = IntegrationRunEntry(
            corpus="integration_cmake",
            index=idx,
            filename_raw=str(source_path.relative_to(cmake_path.parents[1])),
            source_path=source_path,
            extrafiles_raw=extrafiles_raw,
            extrafiles=extrafiles,
            options=_join_options(extra_args),
            llvm=True,
            run=True,
            run_with_dbg=False,
            asr_implicit_interface_and_typing_with_llvm=False,
            pass_with_llvm=False,
            pass_name="",
            fast=False,
            obj=False,
            bin=False,
            name=name,
            labels=labels,
            expected_fail=bool(parsed.get("FAIL", False)),
        )
        idx += 1
        yield entry


def is_llvm_intended(entry: IntegrationRunEntry) -> bool:
    return "llvm" in entry.labels


def is_expected_failure(entry: IntegrationRunEntry) -> bool:
    return entry.expected_fail
