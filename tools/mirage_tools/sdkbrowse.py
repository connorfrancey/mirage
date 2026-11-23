"""Browsing the generated SDK (sdk/include/sdk/*.hpp) from the command line: `mirage sdk-browse`.

The headers are read as sdkgen writes them: every type starts with a comment naming it ("// Class TdGame.TdPawn
(0x08F8 bytes)", "// ScriptStruct ...", "// Enum ..."), then its declaration, then a body that ends with "\t};". Inside
a body, a field carries its offset and size in a comment ("// 0x03C4 (0x0004) (flags)"). Packed Boolean fields carry
their mask instead of the size of the shared 32-bit word. A function is a declaration ending in ");" at the body's own
brace depth (hand-written inline functions' bodies are one level deeper), and an enum value is "NAME = n,".
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

SDK_DIR = Path(__file__).resolve().parents[2] / "sdk" / "include" / "sdk"

_HEAD = re.compile(r"^\t// (Class|ScriptStruct|Enum|Struct) ([\w.]+)(?: \((0x[0-9A-Fa-f]+) bytes(?:, [^)]*)?\))?\s*$")
_DECL = re.compile(r"^\t(?:class|struct|enum class) (\w+)(?:\s*:\s*(?:public )?(\w+))?")
_FIELD = re.compile(r"^\s*(.+?)\s+(\w+(?:\[\w+\])?)\s*;\s*// (0x[0-9A-Fa-f]+) \((0x[0-9A-Fa-f]+)\)(?: \((.*)\))?")
_BIT_FIELD = re.compile(r"^\s*(unsigned long)\s+(\w+)\s*:\s*1;\s*// (0x[0-9A-Fa-f]+) "
                        r"\(bit (0x[0-9A-Fa-f]+)\)(?: \((.*)\))?")
_FUNC = re.compile(r"^\s*(?!return\b)([\w:<>,*&\s]+?[\s*&])(\w+)\((.*)\)\s*(?:const\s*)?;\s*$")
_ENUM = re.compile(r"^\s*(\w+)\s*=\s*(-?\w+),?")
QUALIFIED_MEMBER = re.compile(r"([\w.]+)::(\w+(?:\[\w+\])?)")


@dataclass
class Field:
    type: str
    name: str
    offset: int
    size: int
    flags: str
    bit_mask: int | None = None

    @property
    def declaration(self) -> str:
        width = " : 1" if self.bit_mask is not None else ""
        return f"{self.type} {self.name}{width};"

    @property
    def annotations(self) -> str:
        if self.bit_mask is None:
            return self.flags
        mask = f"bit 0x{self.bit_mask:X}"
        return f"{mask}, {self.flags}" if self.flags else mask


@dataclass
class Type:
    kind: str                   # Class, ScriptStruct, Struct, Enum
    path: str                   # TdGame.TdPawn
    cpp: str                    # ATdPawn
    size: int | None
    super: str | None           # Its base's C++ name
    file: str
    line: int
    fields: list[Field] = field(default_factory=list)
    functions: list[str] = field(default_factory=list)
    values: list[tuple[str, str]] = field(default_factory=list)   # Enum: name, value

    @property
    def name(self) -> str:
        return self.path.rsplit(".", 1)[-1]


def parse_file(path: Path) -> list[Type]:
    types: list[Type] = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        head = _HEAD.match(lines[i])
        decl = _DECL.match(lines[i + 1]) if head and i + 1 < len(lines) else None
        if not decl:
            i += 1
            continue
        t = Type(head[1], head[2], decl[1], int(head[3], 16) if head[3] else None, decl[2], path.name, i + 1)
        depth, i = 0, i + 2
        while i < len(lines) and lines[i] != "\t};":
            line = lines[i]
            if depth == 1 and (f := _FIELD.match(line)):
                t.fields.append(Field(f[1].strip(), f[2], int(f[3], 16), int(f[4], 16), f[5] or ""))
            elif depth == 1 and (f := _BIT_FIELD.match(line)):
                t.fields.append(Field(f[1], f[2], int(f[3], 16), 4, f[5] or "", int(f[4], 16)))
            elif depth == 1 and t.kind == "Enum" and (e := _ENUM.match(line)):
                t.values.append((e[1], e[2]))
            elif depth == 1 and (fn := _FUNC.match(line)):
                t.functions.append(line.strip())
            depth += line.count("{") - line.count("}")
            i += 1
        types.append(t)
    return types


def load(sdk_dir: Path = SDK_DIR) -> list[Type]:
    return [t for f in sorted(sdk_dir.glob("*.hpp")) for t in parse_file(f)]


def find(types: list[Type], query: str) -> list[Type]:
    """Exact matches by C++ name, UnrealScript name or path (any case); else every type whose names contain it."""
    q = query.lower()
    exact = [t for t in types if q in (t.cpp.lower(), t.name.lower(), t.path.lower())]
    return exact or [t for t in types if q in t.cpp.lower() or q in t.path.lower()]


def chain(types: list[Type], t: Type) -> list[Type]:
    """The type and what it extends, nearest first."""
    by_cpp = {x.cpp: x for x in types if x.kind != "Enum"}
    out, seen = [t], {t.cpp}
    while (s := out[-1].super) and s in by_cpp and s not in seen:
        out.append(by_cpp[s])
        seen.add(s)
    return out


def function_name(declaration: str) -> str:
    m = _FUNC.match(declaration)
    return m[2] if m else ""


def find_members(types: list[Type], query: str) -> list[tuple[Type, Field | str]]:
    """Find members by name, or exactly by a qualified Type::Member name."""
    q = query.lower()
    qualified = QUALIFIED_MEMBER.fullmatch(q)
    if qualified:
        owner, q = qualified.groups()
        types = [t for t in types if owner in (t.cpp.lower(), t.name.lower(), t.path.lower())]

    def matches(name: str) -> bool:
        return q == name.lower() if qualified else q in name.lower()

    out: list[tuple[Type, Field | str]] = []
    for t in types:
        out += [(t, f) for f in t.fields if matches(f.name)]
        out += [(t, fn) for fn in t.functions if matches(function_name(fn))]
    return out
