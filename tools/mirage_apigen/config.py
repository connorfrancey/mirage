"""TOML policy and reviewed runtime contracts."""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .sdk import IDENTIFIER, PATH

KEYWORDS = frozenset("and break do else elseif end false for function goto if in local nil not or repeat return then true until while".split())


def lua_identifier(value: str) -> str:
    return value + "_" if value in KEYWORDS else value


@dataclass(frozen=True, slots=True)
class MemberRule:
    expose: bool = False
    name: str = ""
    aliases: tuple[str, ...] = ()
    writable: bool = False
    deprecated: bool = False
    experimental: bool = False
    game_thread: bool = True
    binding: str = "reflection"
    description: str = ""


@dataclass(frozen=True, slots=True)
class APIExposurePolicy:
    version: str = "1.0.0"
    package_allow: tuple[str, ...] = ()
    package_deny: tuple[str, ...] = ()
    class_allow: tuple[str, ...] = ()
    class_deny: tuple[str, ...] = ()
    include_experimental: bool = False
    editor_fallback: str = "omit"
    members: dict[str, MemberRule] = field(default_factory=dict)
    names: dict[str, str] = field(default_factory=dict)
    descriptions: dict[str, str] = field(default_factory=dict)
    types: dict[str, str] = field(default_factory=dict)
    adapters: dict[str, str] = field(default_factory=dict)


def _table(value: object) -> dict:
    if not isinstance(value, dict):
        raise ValueError("Expected a TOML table")
    return value


def _text(value: object, label: str, pattern: re.Pattern[str] | None = None) -> str:
    if not isinstance(value, str) or (pattern and not pattern.fullmatch(value)):
        raise ValueError(f"Invalid {label}")
    return value


def _names(value: object, pattern: re.Pattern[str]) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise ValueError("Expected an array of identifiers")
    values = tuple(sorted(_text(item, "identifier", pattern) for item in value))
    if len(set(values)) != len(values):
        raise ValueError("Duplicate identifiers in configuration")
    return values


def _keys(data: dict, allowed: str) -> None:
    if unknown := data.keys() - set(allowed.split()):
        raise ValueError(f"Unknown configuration keys: {', '.join(sorted(unknown))}")


def _bool(data: dict, key: str, default: bool = False) -> bool:
    value = data.get(key, default)
    if type(value) is not bool:
        raise ValueError(f"{key} must be a Boolean")
    return value


def read_config(path: Path | None) -> APIExposurePolicy:
    if path is None:
        return APIExposurePolicy()
    with path.open("rb") as stream:
        data = tomllib.load(stream)
    _keys(data, "api exposure members naming descriptions types adapters")
    api = _table(data.get("api", {}))
    exposure = _table(data.get("exposure", {}))
    _keys(api, "version editor_fallback")
    _keys(exposure, "package_allow package_deny class_allow class_deny include_experimental")
    fallback = _text(api.get("editor_fallback", "omit"), "editor_fallback")
    if fallback not in ("omit", "any", "unknown"):
        raise ValueError("editor_fallback must be omit, any or unknown")
    members = {}
    for name, value in _table(data.get("members", {})).items():
        _text(name, "member path", PATH)
        rule = _table(value)
        _keys(rule, "expose name aliases writable deprecated experimental game_thread binding description")
        names = _names(rule.get("aliases", []), IDENTIFIER)
        public = _text(rule["name"], "public member name", IDENTIFIER) if "name" in rule else ""
        if public in KEYWORDS or any(alias in KEYWORDS for alias in names) or public in names:
            raise ValueError(f"Conflicting or reserved Lua name for {name}")
        members[name] = MemberRule(
            _bool(rule, "expose"), public, names, _bool(rule, "writable"), _bool(rule, "deprecated"),
            _bool(rule, "experimental"), _bool(rule, "game_thread", True),
            _text(rule.get("binding", "reflection"), "binding", IDENTIFIER),
            _text(rule.get("description", ""), "description"),
        )
    mappings = {}
    for key, pattern in (("naming", PATH), ("descriptions", None), ("types", PATH), ("adapters", PATH)):
        mappings[key] = {_text(path, key, PATH): _text(value, key, pattern)
                         for path, value in _table(data.get(key, {})).items()}
    # Type overrides are editor-only. Qualified names belong to the inspection namespace.
    for value in (*mappings["types"].values(), *mappings["adapters"].values()):
        if value not in {"boolean", "integer", "number", "string", "table", "function", "any", "unknown"}:
            raise ValueError("Type and adapter overrides must name a LuaLS primitive")
    return APIExposurePolicy(
        _text(api.get("version", "1.0.0"), "API version", re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")),
        _names(exposure.get("package_allow", []), IDENTIFIER), _names(exposure.get("package_deny", []), IDENTIFIER),
        _names(exposure.get("class_allow", []), PATH), _names(exposure.get("class_deny", []), PATH),
        _bool(exposure, "include_experimental"), fallback, members, mappings["naming"], mappings["descriptions"],
        mappings["types"], mappings["adapters"],
    )
