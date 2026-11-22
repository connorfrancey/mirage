"""Read generated metadata for the existing SDK browser."""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

from .artifacts import file_path


class BrowserMetadata:
    def __init__(self, output: Path) -> None:
        self.root = output.resolve()
        self.types: dict[str, dict] = {}
        self.members: dict[str, list[tuple[str, dict]]] = defaultdict(list)
        try:
            manifest = json.loads(file_path(self.root, "api-manifest.json").read_text(encoding="utf-8"))
            if not isinstance(manifest, dict) or manifest.get("format") != 1:
                return
            types, members = manifest.get("types"), manifest.get("members")
            if not isinstance(types, dict) or not isinstance(members, dict):
                return
            self.types = {key: value for key, value in types.items() if isinstance(value, dict)}
            for key, member in members.items():
                if isinstance(member, dict) and isinstance(member.get("owner"), str):
                    self.members[member["owner"]].append((key, member))
        except (OSError, ValueError, RecursionError):
            return

    def artifact(self, type_path: str, key: str) -> Path | None:
        relative = self.types.get(type_path, {}).get(key)
        prefixes = {"docs": ("docs/",), "definitions": ("inspection/", "lua/"),
                    "runtime_definitions": ("lua/",)}
        if not isinstance(relative, str) or not relative.startswith(prefixes.get(key, ())):
            return None
        try:
            path = file_path(self.root, relative)
            return path if path.is_file() else None
        except (OSError, ValueError):
            return None
