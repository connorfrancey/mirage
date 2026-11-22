"""Plan, validate and update a directory of generated files."""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MANIFEST = "generation.json"
LOCK = ".mirage-apigen.lock"
FORMAT = 1
OWNED = re.compile(r"(?:api-manifest|diagnostics|coverage)\.json|(?:lua/runtime|inspection/package-[\w]+)\.lua"
                   r"|docs/(?:index|type-[\w.-]+|package-[\w]+)\.md|cpp/package-[\w]+\.cpp", re.ASCII)


def json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, indent=2, sort_keys=True) + "\n"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def fingerprint(value: Any) -> str:
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def file_path(root: Path, relative: str) -> Path:
    if relative != MANIFEST and not OWNED.fullmatch(relative):
        raise ValueError(f"Invalid generated file path: {relative!r}")
    path = root / relative
    for part in (path, *path.parents):
        if part == root:
            break
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise ValueError(f"Generated path crosses a link: {part}")
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"Generated path escapes output directory: {relative}")
    return path


def previous_manifest(root: Path) -> dict:
    manifest = file_path(root, MANIFEST)
    if not manifest.exists():
        return {}
    result = json.loads(manifest.read_text(encoding="utf-8"))
    if (not isinstance(result, dict) or result.get("generator") != "MirageAPIGen"
            or result.get("format") != FORMAT or not isinstance(result.get("files"), dict)):
        raise ValueError(f"Invalid MirageAPIGen generation manifest: {manifest}")
    for path, value in result["files"].items():
        if path == MANIFEST:
            raise ValueError("The generation manifest cannot track itself")
        file_path(root, path)
        if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{64}", value):
            raise ValueError(f"Invalid content hash for {path}")
    return result


@contextmanager
def output_lock(root: Path) -> Iterator[None]:
    root.mkdir(parents=True, exist_ok=True)
    path = root / LOCK
    if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
        raise ValueError(f"Generation lock crosses a link: {path}")
    with path.open("a+b") as stream:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt

            acquire = lambda: msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            acquire = lambda: fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            acquire()
        except OSError as exc:
            raise ValueError(f"Another MirageAPIGen writer is using {root}") from exc
        yield
        # Closing the descriptor releases the advisory lock even if the writer fails.


@dataclass(slots=True)
class Plan:
    writes: dict[str, bytes]
    removes: list[str]


def plan_output(root: Path, files: dict[str, str], metadata: dict, *, force: bool = False,
                clean: bool = False, check_only: bool = False) -> Plan:
    old_files = previous_manifest(root).get("files", {})
    if len({name.casefold() for name in files}) != len(files):
        raise ValueError("Generated filenames collide on a case-insensitive filesystem")
    data = {name: text.encode() for name, text in files.items()}
    if not clean:
        data[MANIFEST] = json_text({"generator": "MirageAPIGen", "format": FORMAT, **metadata,
                                    "files": {name: digest(value) for name, value in data.items()}}).encode()
    writes = {}
    removes = []
    for name, content in sorted(data.items(), key=lambda item: (item[0] == MANIFEST, item[0])):
        path = file_path(root, name)
        before = path.read_bytes() if path.exists() else None
        if not check_only and name != MANIFEST and before is not None and before != content and name not in old_files:
            raise ValueError(f"Existing file is outside the generation manifest: {path}")
        if force or before != content:
            writes[name] = content
    for name in sorted(old_files.keys() - files.keys()):
        path = file_path(root, name)
        if path.exists():
            if not (force or check_only) and digest(path.read_bytes()) != old_files[name]:
                raise ValueError(f"Obsolete generated file was modified: {path}. Use --force to remove it.")
            removes.append(name)
    if clean and file_path(root, MANIFEST).exists():
        removes.append(MANIFEST)
    return Plan(writes, removes)


def _replace(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".apigen-", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(data)
            stream.close()
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def apply_plan(root: Path, plan: Plan) -> None:
    for name, data in plan.writes.items():
        if name != MANIFEST:
            _replace(file_path(root, name), data)
    for name in plan.removes:
        file_path(root, name).unlink(missing_ok=True)
    if MANIFEST in plan.writes:
        _replace(file_path(root, MANIFEST), plan.writes[MANIFEST])
