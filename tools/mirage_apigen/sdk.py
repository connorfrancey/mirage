"""Normalized UE3 layout metadata and indexed relationships."""

from __future__ import annotations

import json
import re
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,127}")
PATH = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,127}(?:\.[A-Za-z_][A-Za-z0-9_]{0,127}){0,31}")


@dataclass(frozen=True, slots=True)
class Diagnostic:
    severity: str
    code: str
    subject: str
    message: str


@dataclass(frozen=True, slots=True)
class SDKTypeReference:
    kind: str
    path: str = ""
    inner: SDKTypeReference | None = None
    dimension: int = 1
    key: SDKTypeReference | None = None

    def paths(self) -> set[str]:
        paths = {self.path} if self.path else set()
        for ref in (self.inner, self.key):
            if ref:
                paths.update(ref.paths())
        return paths


@dataclass(frozen=True, slots=True)
class SDKProperty:
    name: str
    type: SDKTypeReference
    offset: int | None
    size: int | None
    flags: int | None
    mask: int | None


@dataclass(frozen=True, slots=True)
class SDKParameter:
    property: SDKProperty
    direction: str
    optional: bool = False


@dataclass(frozen=True, slots=True)
class SDKFunction:
    path: str
    owner: str
    flags: int | None
    native_index: int | None
    size: int | None
    parameters: tuple[SDKParameter, ...]
    strategy: str
    verified: bool
    signature_known: bool
    super: str = ""


@dataclass(slots=True)
class SDKClass:
    path: str
    super: str
    size: int | None
    properties: tuple[SDKProperty, ...]
    functions: list[SDKFunction] = field(default_factory=list)


@dataclass(slots=True)
class SDKStruct:
    path: str
    kind: str
    super: str
    size: int | None
    properties: tuple[SDKProperty, ...]


@dataclass(slots=True)
class SDKEnum:
    path: str
    values: dict[str, int]
    size: int | None


SDKType = SDKClass | SDKStruct | SDKEnum


@dataclass(slots=True)
class SDKPackage:
    name: str
    types: list[SDKType] = field(default_factory=list)


@dataclass(slots=True)
class SDKIndex:
    types: dict[str, SDKType] = field(default_factory=dict)
    packages: dict[str, SDKPackage] = field(default_factory=dict)
    functions: dict[str, SDKFunction] = field(default_factory=dict)
    states: set[str] = field(default_factory=set)
    dependencies: dict[str, set[str]] = field(default_factory=dict)
    dependents: dict[str, set[str]] = field(default_factory=dict)
    parents: dict[str, str] = field(default_factory=dict)
    diagnostics: list[Diagnostic] = field(default_factory=list)

    def diagnose(self, code: str, subject: str, message: str, severity: str = "warning") -> None:
        self.diagnostics.append(Diagnostic(severity, code, subject, message))

    def analyze(self) -> None:
        for path, entry in self.types.items():
            deps: set[str] = set()
            if not isinstance(entry, SDKEnum):
                if entry.super:
                    parent = self.types.get(entry.super)
                    if type(parent) is type(entry):
                        self.parents[path] = entry.super
                        deps.add(entry.super)
                    else:
                        self.diagnose("missing-parent", path, f"Parent {entry.super} is missing or has the wrong kind.")
                for prop in entry.properties:
                    deps.update(prop.type.paths())
                if isinstance(entry, SDKClass):
                    for fn in entry.functions:
                        for param in fn.parameters:
                            deps.update(param.property.type.paths())
            for ref in sorted(deps - self.types.keys()):
                self.diagnose("missing-reference", path, f"Type {ref} is absent from this dump.")
            self.dependencies[path] = deps
            for dep in deps:
                self.dependents.setdefault(dep, set()).add(path)
        visited: set[str] = set()
        for path in self.parents.copy():
            chain: dict[str, None] = {}
            current = path
            while current in self.parents and current not in visited:
                if current in chain:
                    cycle = list(chain)[list(chain).index(current):]
                    self.diagnose("inheritance-cycle", current, " -> ".join([*cycle, current]), "error")
                    for member in cycle:
                        del self.parents[member]
                    break
                chain[current] = None
                current = self.parents[current]
            visited.update(chain)


def _identifier(value: Any, pattern: re.Pattern[str] = IDENTIFIER) -> str:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise ValueError("Invalid identifier")
    return value


def _optional_path(value: Any) -> str:
    return "" if value in ("", None) else _identifier(value, PATH)


def _integer(value: Any, *, minimum: int = 0, maximum: int = 2**64 - 1) -> int | None:
    if value is not None and (type(value) is not int or not minimum <= value <= maximum):
        raise ValueError("Integer metadata is outside the supported range")
    return value


def _sequence(value: Any) -> list:
    if not isinstance(value, list):
        raise ValueError("Expected an array of SDK records")
    return value


def _ref(raw: dict, depth: int = 0) -> SDKTypeReference:
    if depth >= 32:
        raise ValueError("Type nesting exceeds 32 levels")
    kind = _identifier(raw.get("kind", "Unknown"))
    inner = raw.get("inner", raw.get("value"))
    if isinstance(inner, str):
        inner = {"kind": inner, "type": raw.get("innerType")}
    key = raw.get("key")
    if any(value is not None and not isinstance(value, dict) for value in (inner, key)):
        raise ValueError("Expected a type descriptor")
    return SDKTypeReference(kind, _optional_path(raw.get("type")),
                            _ref(inner, depth + 1) if inner is not None else None,
                            _integer(raw.get("dim", raw.get("arrayDim", 1)), minimum=1) or 1,
                            _ref(key, depth + 1) if key is not None else None)


def _property(raw: dict) -> SDKProperty:
    if not isinstance(raw, dict):
        raise ValueError("Expected a property object")
    return SDKProperty(_identifier(raw.get("name")), _ref(raw), _integer(raw.get("offset")),
                       _integer(raw.get("size")), _integer(raw.get("flags")), _integer(raw.get("mask")))


def _properties(raw: list, path: str, index: SDKIndex) -> tuple[SDKProperty, ...]:
    results = []
    names: set[str] = set()
    for pos, value in enumerate(raw):
        try:
            prop = _property(value)
            if prop.name.casefold() in names:
                index.diagnose("duplicate-member", f"{path}.{prop.name}", "Duplicate property.", "error")
                continue
            names.add(prop.name.casefold())
            results.append(prop)
        except ValueError as exc:
            index.diagnose("invalid-property", f"{path}[{pos}]", str(exc), "error")
    return tuple(sorted(results, key=lambda p: p.name))


def _function(raw: dict, owner: str = "") -> SDKFunction:
    path = _identifier(raw.get("path") or f"{owner}.{raw.get('name', '')}", PATH)
    parent, _, name = path.rpartition(".")
    if owner and parent != owner:
        raise ValueError("Nested function belongs to a different class")
    owner = _identifier(raw.get("owner", owner or parent), PATH)
    if parent != owner or raw.get("name", name) != name:
        raise ValueError("Function owner and name must agree with its full path")
    params: list[SDKParameter] = []
    known = "parameters" in raw
    if known:
        records = _sequence(raw["parameters"]).copy()
        if raw.get("return") is not None:
            if not isinstance(raw["return"], dict):
                raise ValueError("Expected a return property")
            records.append({**raw["return"], "direction": "return"})
        for record in records:
            prop = _property(record)
            direction = record.get("direction", "in")
            if direction not in ("in", "out", "inout", "return"):
                raise ValueError("Invalid parameter direction")
            params.append(SDKParameter(prop, direction, record.get("optional") is True))
    elif "properties" in raw:
        # CPF_Parm, CPF_OutParm, CPF_ReturnParm and CPF_OptionalParm match sdkgen/emit_lua.cpp.
        known = True
        for record in _sequence(raw["properties"]):
            prop = _property(record)
            if prop.flags is None:
                known = False
            if prop.flags and prop.flags & 0x80:
                direction = "return" if prop.flags & 0x400 else "inout" if prop.flags & 0x100 else "in"
                params.append(SDKParameter(prop, direction, bool(prop.flags & 0x10)))
    if len({p.property.name.casefold() for p in params}) != len(params):
        raise ValueError("Duplicate parameters")
    if sum(p.direction in ("in", "inout") for p in params) > 199:
        raise ValueError("Function exceeds the Lua parameter limit")
    if sum(p.direction == "return" for p in params) > 1:
        raise ValueError("Multiple return parameters")
    strategy = _identifier(raw.get("invocation", "unknown"))
    return SDKFunction(path, owner, _integer(raw.get("functionFlags", raw.get("flags"))),
                       _integer(raw.get("nativeIndex")), _integer(raw.get("size")), tuple(params),
                       strategy, raw.get("verified") is True, known, _optional_path(raw.get("super")))


def _enum_values(values: Any) -> dict[str, int]:
    records = values.items() if isinstance(values, dict) else (
        (v["name"], v["value"]) if isinstance(v, dict) else (v, i) for i, v in enumerate(_sequence(values))
    )
    result: dict[str, int] = {}
    seen: set[str] = set()
    for key, value in records:
        name = _identifier(key)
        if name.casefold() in seen or value is None:
            raise ValueError("Duplicate or missing enum value")
        seen.add(name.casefold())
        result[name] = _integer(value, minimum=-(2**63), maximum=2**63 - 1)
    return dict(sorted(result.items()))


def _object(pairs: list[tuple[str, Any]]) -> dict:
    result = dict(pairs)
    if len(result) != len(pairs):
        raise ValueError("Duplicate JSON object key")
    return result


def load(path: Path) -> SDKIndex:
    with path.open(encoding="utf-8-sig") as stream:
        return parse(json.load(stream, object_pairs_hook=_object))


def parse(records: Any) -> SDKIndex:
    index = SDKIndex()
    functions: list[tuple[dict, str]] = []
    seen: set[str] = set()
    for pos, raw in enumerate(_sequence(records)):
        subject = f"record[{pos}]"
        try:
            if not isinstance(raw, dict):
                raise ValueError("Expected an SDK record")
            path = _identifier(raw.get("path"), PATH)
            subject = path
            if "." not in path:
                raise ValueError("Expected a package-qualified SDK path")
            kind = raw.get("kind")
            if kind == "Function":
                functions.append((raw, ""))
                continue
            if path.casefold() in seen:
                index.diagnose("duplicate-type", path, "Duplicate SDK identifier.", "error")
                continue
            seen.add(path.casefold())
            size = _integer(raw.get("size"))
            if kind == "Enum":
                index.types[path] = SDKEnum(path, _enum_values(raw.get("values", {})), size)
            elif kind in ("Class", "Struct", "ScriptStruct"):
                parent = _optional_path(raw.get("super"))
                props = _properties(_sequence(raw.get("properties", [])), path, index)
                if kind == "Class":
                    index.types[path] = SDKClass(path, parent, size, props)
                    functions.extend((fn, path) for fn in _sequence(raw.get("functions", [])))
                else:
                    index.types[path] = SDKStruct(path, kind, parent, size, props)
            elif kind == "State":
                index.states.add(path)
            else:
                index.diagnose("unsupported-record", path, "Only class, struct, enum and function records are indexed.", "info")
        except (ValueError, KeyError) as exc:
            index.diagnose("invalid-record", subject, str(exc), "error")

    names: set[str] = set()
    for raw, owner in functions:
        try:
            if not isinstance(raw, dict):
                raise ValueError("Expected a function object")
            fn = _function(raw, owner)
            if fn.path.casefold() in names or fn.path.casefold() in seen:
                index.diagnose("duplicate-function", fn.path, "Duplicate function identifier.", "error")
                continue
            names.add(fn.path.casefold())
            index.functions[fn.path] = fn
            entry = index.types.get(fn.owner)
            if isinstance(entry, SDKClass):
                entry.functions.append(fn)
            elif fn.owner in index.states:
                index.diagnose("state-function", fn.path, "State-scoped functions are excluded.", "info")
            else:
                index.diagnose("missing-owner", fn.path, f"Declaring class {fn.owner} is absent from this dump.")
            if not fn.signature_known:
                index.diagnose("missing-signature", fn.path, "Parameter directions and signature are unavailable.")
        except ValueError as exc:
            index.diagnose("invalid-function", owner or "function", str(exc), "error")
    if not index.functions:
        index.diagnose("missing-functions", "SDK", "The input contains no function metadata.")

    index.types = dict(sorted(index.types.items()))
    for path, entry in index.types.items():
        pkg = path.partition(".")[0]
        index.packages.setdefault(pkg, SDKPackage(pkg)).types.append(entry)
        if isinstance(entry, SDKClass):
            entry.functions.sort(key=lambda fn: fn.path)
    index.analyze()
    return index
