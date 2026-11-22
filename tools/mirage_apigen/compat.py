"""Compatibility comparison for public API manifests and SDK metadata."""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

from .sdk import SDKEnum, parse


def _read(path: Path) -> tuple[str, str | None, dict]:
    with path.open(encoding="utf-8-sig") as stream:
        value = json.load(stream)
    if isinstance(value, list):
        index = parse(value)
        if errors := [d for d in index.diagnostics if d.severity == "error"]:
            raise ValueError(f"{path}: {errors[0].subject} {errors[0].message}")
        records: dict = {}
        for key, entry in index.types.items():
            if isinstance(entry, SDKEnum):
                records[key] = {"kind": "Enum", "size": entry.size}
                for name, number in entry.values.items():
                    records[f"{key}.{name}"] = {"value": number}
            else:
                records[key] = {"kind": type(entry).__name__, "super": entry.super, "size": entry.size}
                for prop in entry.properties:
                    records[f"{key}.{prop.name}"] = asdict(prop)
        records.update((key, asdict(fn)) for key, fn in index.functions.items())
        return "sdk", None, records
    if (not isinstance(value, dict) or value.get("format") != 1 or not isinstance(value.get("api_version"), str)
            or not isinstance(value.get("functions"), dict)):
        raise ValueError(f"Expected an SDK array or API manifest in {path}")
    return "runtime", value["api_version"], value["functions"]


def compare(old: Path, new: Path) -> dict:
    old_kind, old_version, before = _read(old)
    new_kind, new_version, after = _read(new)
    if old_kind != new_kind:
        raise ValueError("Compare two public API manifests or two SDK arrays")
    changes = []
    for name in sorted(before.keys() | after.keys()):
        if name not in after:
            changes.append({"name": name, "change": "removed", "breaking": True})
        elif name not in before:
            changes.append({"name": name, "change": "added", "breaking": False})
        elif before[name] != after[name]:
            if old_kind == "sdk":
                changes.append({"name": name, "change": "metadata-changed", "breaking": True})
                continue
            if not isinstance(before[name], dict) or not isinstance(after[name], dict):
                raise ValueError(f"Malformed function entry: {name}")
            keys = ("parameters", "returns", "source", "game_thread")
            signature_changed = any(before[name].get(key) != after[name].get(key) for key in keys)
            changes.append({"name": name, "change": "signature-changed" if signature_changed else "status-changed",
                            "breaking": signature_changed})
    return {"kind": old_kind, "from_version": old_version, "to_version": new_version,
            "breaking": any(change["breaking"] for change in changes), "changes": changes}
