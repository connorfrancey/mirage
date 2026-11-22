"""Resolve SDK references and project explicitly reviewed Lua bindings."""

from __future__ import annotations

from dataclasses import dataclass, field

from .config import APIExposurePolicy, MemberRule, lua_identifier
from .sdk import Diagnostic, SDKClass, SDKEnum, SDKFunction, SDKIndex, SDKProperty, SDKStruct, SDKTypeReference

PRIMITIVES = {
    "Bool": "boolean", "Byte": "integer", "Int": "integer", "Int64": "integer", "Float": "number",
    "Double": "number", "Str": "string", "Name": "string", "Delegate": "function",
}
REFERENCE_TYPES = {
    "Object": SDKClass, "Class": SDKClass, "Component": SDKClass, "Interface": SDKClass,
    "Struct": SDKStruct, "Enum": SDKEnum, "Byte": SDKEnum,
}


@dataclass(frozen=True, slots=True)
class Binding:
    kind: str
    lua_type: str
    cpp_type: str
    writable: bool = False
    wrapper: str = ""


# Approval comes from source review. SDK paths, offsets and user configuration cannot add entries.
REVIEWED_PROPERTIES = {
    "ALAudio.ALAudioDevice.MinOggVorbisDurationGame": Binding("Float", "number", "MIRAGE_VALUE_FLOAT"),
    "ALAudio.ALAudioDevice.MinOggVorbisDurationEditor": Binding("Float", "number", "MIRAGE_VALUE_FLOAT"),
}


@dataclass(frozen=True, slots=True)
class APIProperty:
    owner: str
    source: SDKProperty
    names: tuple[str, ...]
    rule: MemberRule
    binding: Binding


@dataclass(frozen=True, slots=True)
class APIFunction:
    owner: str
    source: SDKFunction
    names: tuple[str, ...]
    rule: MemberRule
    wrapper: str
    signature: tuple[tuple[str, str, str, bool], ...]


@dataclass(frozen=True, slots=True)
class FunctionBinding:
    source: SDKFunction
    wrapper: str
    signature: tuple[tuple[str, str, str, bool], ...]


# A reviewed function wrapper must implement the Lua stack contract declared here.
REVIEWED_FUNCTIONS: dict[str, FunctionBinding] = {}


@dataclass(slots=True)
class APIClass:
    path: str
    properties: list[APIProperty] = field(default_factory=list)
    functions: list[APIFunction] = field(default_factory=list)


@dataclass(slots=True)
class APIModule:
    version: str
    classes: dict[str, APIClass] = field(default_factory=dict)
    members: dict[str, dict] = field(default_factory=dict)
    types: dict[str, dict] = field(default_factory=dict)
    diagnostics: list[Diagnostic] = field(default_factory=list)


class TypeResolver:
    def __init__(self, index: SDKIndex, policy: APIExposurePolicy, selected: set[str] | None = None) -> None:
        self.index = index
        self.policy = policy
        self.selected = selected
        self.names = {path: "MirageSDK." + ".".join(lua_identifier(part) for part in
                      policy.names.get(path, path).split(".")) for path in index.types}
        self.cache: dict[SDKTypeReference, tuple[str | None, str]] = {}

    def resolve(self, ref: SDKTypeReference) -> tuple[str | None, str]:
        if ref not in self.cache:
            value, issue = self._single(ref)
            if value is None:
                value = None if self.policy.editor_fallback == "omit" else self.policy.editor_fallback
            self.cache[ref] = (f"{value}[]" if value and ref.dimension > 1 else value), issue
        return self.cache[ref]

    def _single(self, ref: SDKTypeReference) -> tuple[str | None, str]:
        if ref.kind == "Array":
            if not ref.inner:
                return None, "Array element metadata is missing."
            value, issue = self.resolve(ref.inner)
            return (f"{value}[]" if value else None), issue
        if ref.kind == "Map":
            if not ref.key or not ref.inner:
                return None, "Map key or value metadata is missing."
            key, ke = self.resolve(ref.key)
            val, ve = self.resolve(ref.inner)
            return (f"table<{key}, {val}>" if key and val else None), ke or ve
        expected = REFERENCE_TYPES.get(ref.kind)
        if expected and ref.path and not isinstance(self.index.types.get(ref.path), expected):
            if ref.path not in self.policy.adapters:
                return None, f"{ref.kind} reference {ref.path} is missing or has the wrong kind."
        if ref.path in self.policy.adapters:
            return self.policy.adapters[ref.path], ""
        if ref.kind in self.policy.types:
            return self.policy.types[ref.kind], ""
        if expected and ref.path:
            if self.selected is not None and ref.path not in self.selected:
                return ("integer" if expected is SDKEnum else "table"), ""
            return self.names.get(ref.path), ""
        if ref.kind in PRIMITIVES:
            return PRIMITIVES[ref.kind], ""
        if ref.kind in ("Object", "Class", "Component", "Interface"):
            return "table", ""
        return None, f"No type mapping for {ref.kind}."

    def signature(self, fn: SDKFunction) -> tuple[tuple[tuple[str, str, str, bool], ...] | None, str]:
        if not fn.signature_known:
            return None, "Function signature metadata is absent."
        params = []
        issues = []
        for param in fn.parameters:
            type_name, issue = self.resolve(param.property.type)
            if type_name is None:
                return None, issue
            if issue:
                issues.append(issue)
            params.append((param.property.name, type_name, param.direction, param.optional))
        return tuple(params), " ".join(issues)


def analyze(index: SDKIndex, resolver: TypeResolver, package: str = "", cls: str = "") -> APIModule:
    policy = resolver.policy
    diagnostics = [d for d in index.diagnostics if d.severity == "error" or d.subject == "SDK" or
                   ((not package or d.subject.partition(".")[0] == package) and
                    (not cls or d.subject == cls or d.subject.startswith(cls + ".")))]
    api = APIModule(policy.version, diagnostics=diagnostics)
    used_names: dict[str, str] = {}
    type_paths: dict[str, str] = {}
    available: set[str] = set()
    package_allow, package_deny = set(policy.package_allow), set(policy.package_deny)
    class_allow, class_deny = set(policy.class_allow), set(policy.class_deny)
    for first, second in ((package_allow, package_deny), (class_allow, class_deny)):
        for path in sorted(first & second):
            api.diagnostics.append(Diagnostic("error", "conflicting-policy", path, "Identifier is both allowed and denied."))
    for path, name in resolver.names.items():
        if other := type_paths.get(name):
            api.diagnostics.append(Diagnostic("error", "duplicate-lua-type", path, f"{name} also names {other}."))
        type_paths[name] = path
    for path, entry in index.types.items():
        pkg = path.partition(".")[0]
        if (package and pkg != package) or (cls and path != cls):
            continue
        api.types[path] = {"lua_name": resolver.names[path], "kind": type(entry).__name__[3:].lower(),
                           "super": index.parents.get(path), "docs": None, "definitions": None}
        if isinstance(entry, SDKEnum):
            continue
        allowed = ((not package_allow or pkg in package_allow) and pkg not in package_deny
                   and (not class_allow or path in class_allow) and path not in class_deny)
        items = [(f"{path}.{prop.name}", prop) for prop in entry.properties]
        if isinstance(entry, SDKClass):
            items.extend((fn.path, fn) for fn in entry.functions)
        declared: set[str] = set()
        for full, member in items:
            if full.casefold() in declared:
                api.diagnostics.append(Diagnostic("error", "duplicate-member", full, "Properties and functions share this identifier."))
                continue
            declared.add(full.casefold())
            available.add(full)
            is_property = isinstance(member, SDKProperty)
            editor, editor_issue = (resolver.resolve(member.type) if is_property else resolver.signature(member))
            if editor_issue:
                api.diagnostics.append(Diagnostic("warning", "unsupported-editor-type", full, editor_issue))
            rule = policy.members.get(full, MemberRule())
            binding = REVIEWED_PROPERTIES.get(full) if isinstance(entry, SDKClass) else None
            reviewed_fn = REVIEWED_FUNCTIONS.get(full)
            eligible = bool(
                (is_property and binding and member.type.kind == binding.kind and member.type.dimension == 1
                 and not member.type.path and not member.type.inner)
                or (not is_property and reviewed_fn and member.verified and member.signature_known
                    and member == reviewed_fn.source)
            )
            requested = rule.expose
            exposed = requested and allowed and (policy.include_experimental or not rule.experimental)
            reason = ""
            unsafe = ""
            if not exposed:
                reason = ("No explicit exposure permission." if not requested else "Excluded by package, class or experimental policy.")
            elif not eligible:
                unsafe = ("Property has no reviewed runtime contract for this type." if is_property
                          else "No verified invocation wrapper and matching function metadata.")
            elif not rule.game_thread:
                unsafe = "Engine access requires the game thread."
            elif rule.writable and (not is_property or not binding.writable):
                unsafe = "This binding has no reviewed write support."
            elif rule.binding != ((binding.wrapper or "reflection") if is_property else reviewed_fn.wrapper):
                unsafe = "The requested binding does not match the reviewed runtime contract."
            if unsafe:
                reason = unsafe
                api.diagnostics.append(Diagnostic("warning", "unsafe-binding", full, reason))
            names: tuple[str, ...] = ()
            public: list[str] = []
            if not reason:
                names = (rule.name or full.replace(".", "_"), *rule.aliases)
                public = [name for stem in names for name in (
                    (f"get_{stem}", f"set_{stem}") if rule.writable else (f"get_{stem}",) if is_property else (stem,)
                )]
                if len(set(public)) != len(public) or any(name in used_names for name in public):
                    api.diagnostics.append(Diagnostic("error", "duplicate-lua-name", full, "Public runtime name collides."))
                    reason, names = "Public name collision.", ()
                else:
                    used_names.update(dict.fromkeys(public, full))
                    api_class = api.classes.setdefault(path, APIClass(path))
                    if is_property:
                        api_class.properties.append(APIProperty(path, member, names, rule, binding))
                    else:
                        api_class.functions.append(APIFunction(path, member, names, rule, reviewed_fn.wrapper, reviewed_fn.signature))
            if reason:
                api.diagnostics.append(Diagnostic("info", "excluded-member", full, reason))
            api.members[full] = {
                "owner": path, "kind": "property" if is_property else "function", "eligible": eligible,
                "exposed": exposed, "generated": bool(names), "reason": reason, "editor": editor is not None,
                "editor_issue": editor_issue, "type": binding.lua_type if names and binding else None,
                "writable": rule.writable if names else False,
                "lua_names": [f"mirage.sdk.{name}" for name in public] if names else [],
            }
    for full, fn in sorted(index.functions.items()):
        if fn.owner in index.types or (cls and fn.owner != cls) or (package and fn.owner.partition(".")[0] != package):
            continue
        available.add(full)
        reason = ("State-scoped functions are excluded." if fn.owner in index.states
                  else "Declaring class is absent from the dump.")
        api.members[full] = {
            "owner": fn.owner, "kind": "function", "eligible": False, "exposed": False, "generated": False,
            "reason": reason, "editor": False,
            "editor_issue": reason, "type": None, "writable": False, "lua_names": [],
        }
    if not (package or cls):
        for path in sorted(policy.members.keys() - available):
            api.diagnostics.append(Diagnostic("warning", "unknown-policy-member", path, "Exposure rule matches no member."))
    return api
