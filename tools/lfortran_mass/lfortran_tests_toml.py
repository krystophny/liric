#!/usr/bin/env python3
"""Parse and resolve LFortran tests.toml entries for mass testing."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


@dataclass
class LFortranTestEntry:
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

    def to_manifest_record(self, lfortran_version: str) -> Dict[str, Any]:
        source_hash = file_sha256(self.source_path)
        case_id = stable_case_id(
            str(self.source_path), source_hash, lfortran_version, self.recipe_kind()
        )
        return {
            "case_id": case_id,
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
        }

    def recipe_kind(self) -> str:
        if self.pass_with_llvm and self.pass_name:
            return "pass_with_llvm"
        if self.asr_implicit_interface_and_typing_with_llvm:
            return "asr_implicit_interface_and_typing_with_llvm"
        if self.llvm:
            return "llvm"
        return "fallback_llvm"


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def stable_case_id(
    source_path: str, source_hash: str, lfortran_version: str, recipe_kind: str
) -> str:
    payload = "|".join([source_path, source_hash, lfortran_version, recipe_kind])
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]


def _strip_comment(line: str) -> str:
    in_quote = False
    out_chars: List[str] = []
    for ch in line:
        if ch == '"':
            in_quote = not in_quote
        if ch == "#" and not in_quote:
            break
        out_chars.append(ch)
    return "".join(out_chars)


def _parse_value(raw: str) -> Any:
    value = raw.strip()
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    if value.lower() == "true":
        return True
    if value.lower() == "false":
        return False
    if re.fullmatch(r"-?[0-9]+", value):
        return int(value)
    return value


def _parse_toml_fallback(text: str) -> Dict[str, Any]:
    tests: List[Dict[str, Any]] = []
    current: Optional[Dict[str, Any]] = None

    for raw_line in text.splitlines():
        line = _strip_comment(raw_line).strip()
        if not line:
            continue
        if line == "[[test]]":
            if current is not None:
                tests.append(current)
            current = {}
            continue
        if current is None or "=" not in line:
            continue

        key, value = [x.strip() for x in line.split("=", 1)]
        current[key] = _parse_value(value)

    if current is not None:
        tests.append(current)
    return {"test": tests}


def load_tests_toml(path: Path) -> Dict[str, Any]:
    raw_bytes = path.read_bytes()
    data: Optional[Dict[str, Any]] = None

    try:
        import tomllib  # type: ignore

        data = tomllib.loads(raw_bytes.decode("utf-8"))
    except ModuleNotFoundError:
        try:
            import tomli  # type: ignore

            data = tomli.loads(raw_bytes.decode("utf-8"))
        except ModuleNotFoundError:
            data = _parse_toml_fallback(raw_bytes.decode("utf-8"))

    if not isinstance(data, dict) or "test" not in data:
        raise ValueError(f"Invalid tests.toml format in {path}")
    return data


def _split_csv(value: str) -> List[str]:
    if not value:
        return []
    parts = [x.strip() for x in value.split(",")]
    return [x for x in parts if x]


def _resolve_path(raw: str, lfortran_root: Path, tests_dir: Path) -> Path:
    p1 = (tests_dir / raw).resolve()
    if p1.exists():
        return p1
    p2 = (lfortran_root / raw).resolve()
    if p2.exists():
        return p2
    return p1


def iter_entries(tests_toml: Path, lfortran_root: Path) -> Iterable[LFortranTestEntry]:
    data = load_tests_toml(tests_toml)
    tests = data.get("test", [])
    tests_dir = tests_toml.parent

    for idx, raw in enumerate(tests):
        if not isinstance(raw, dict):
            continue
        filename = str(raw.get("filename", "")).strip()
        if not filename:
            continue

        source_path = _resolve_path(filename, lfortran_root, tests_dir)
        extrafiles_raw = _split_csv(str(raw.get("extrafiles", "")))
        extrafiles = [
            _resolve_path(extra, lfortran_root, tests_dir) for extra in extrafiles_raw
        ]

        entry = LFortranTestEntry(
            index=idx,
            filename_raw=filename,
            source_path=source_path,
            extrafiles_raw=extrafiles_raw,
            extrafiles=extrafiles,
            options=str(raw.get("options", "")).strip(),
            llvm=bool(raw.get("llvm", False)),
            run=bool(raw.get("run", False)),
            run_with_dbg=bool(raw.get("run_with_dbg", False)),
            asr_implicit_interface_and_typing_with_llvm=bool(
                raw.get("asr_implicit_interface_and_typing_with_llvm", False)
            ),
            pass_with_llvm=bool(raw.get("pass_with_llvm", False)),
            pass_name=str(raw.get("pass", "")).strip(),
            fast=bool(raw.get("fast", False)),
        )
        yield entry


def entries_to_manifest(
    tests_toml: Path, lfortran_root: Path, lfortran_version: str
) -> List[Dict[str, Any]]:
    manifest: List[Dict[str, Any]] = []
    for entry in iter_entries(tests_toml, lfortran_root):
        manifest.append(entry.to_manifest_record(lfortran_version))
    return manifest


def write_jsonl(path: Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=True, sort_keys=True))
            f.write("\n")
