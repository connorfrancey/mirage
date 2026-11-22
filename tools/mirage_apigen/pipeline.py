"""Analyze once and render the selected outputs from a shared API contract."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
from time import perf_counter

from .analysis import APIModule, TypeResolver, analyze
from .artifacts import fingerprint, json_text
from .config import read_config
from .docs import docs, type_page
from .render import inspection, luals, public_functions
from .sdk import load

TARGETS = ("luals", "cpp", "docs")


@dataclass(slots=True)
class Generation:
    api: APIModule
    coverage: dict
    public: dict
    files: dict[str, str]
    metadata: dict
    timings: dict[str, float]


def coverage(api: APIModule) -> dict:
    def counts(members: list[dict]) -> dict:
        return {
            "runtime": {"discovered": len(members), "eligible": sum(m["eligible"] for m in members),
                        "exposed": sum(m["exposed"] for m in members), "generated": sum(m["generated"] for m in members),
                        "skipped": sum(not m["generated"] for m in members),
                        "unsupported": sum(not m["eligible"] for m in members)},
            "editor": {"discovered": len(members), "supported": sum(m["editor"] and not m["editor_issue"] for m in members),
                       "fallback": sum(m["editor"] and bool(m["editor_issue"]) for m in members),
                       "skipped": sum(not m["editor"] for m in members)},
        }

    packages = {path.partition(".")[0]: [] for path in api.types}
    for member in api.members.values():
        packages.setdefault(member["owner"].partition(".")[0], []).append(member)
    return {**counts(list(api.members.values())), "packages": {pkg: counts(members) for pkg, members in packages.items()}}


def generate(input_path: Path, config: Path | None, targets: tuple[str, ...], *, package: str = "", cls: str = "",
             include_inspection: bool = False) -> Generation:
    start = perf_counter()
    policy = read_config(config)
    index = load(input_path)
    parsed = perf_counter()
    if package and package not in index.packages:
        raise ValueError(f"Package not in SDK: {package}")
    if cls and (cls not in index.types or (package and cls.partition(".")[0] != package)):
        raise ValueError(f"Type not in SDK selection: {cls}")
    selected = ({cls} if cls else {entry.path for entry in index.packages[package].types} if package else None)
    resolver = TypeResolver(index, policy, selected)
    api = analyze(index, resolver, package, cls)
    analyzed = perf_counter()
    functions = list(public_functions(api))
    files: dict[str, str] = {}
    if "luals" in targets:
        if functions:
            files["lua/runtime.lua"] = luals(functions)
        if include_inspection:
            files.update(inspection(api, resolver))
    if "docs" in targets:
        files.update(docs(api, resolver, functions, include_inspection))
    if "cpp" in targets:
        from .cpp import cpp

        files.update(cpp(functions))
    for path, info in api.types.items():
        page = type_page(path)
        info["docs"] = page if page in files else None
        definition = f"inspection/package-{path.partition('.')[0]}.lua"
        info["runtime_definitions"] = "lua/runtime.lua" if path in api.classes and "lua/runtime.lua" in files else None
        info["definitions"] = (definition if definition in files else
                               info["runtime_definitions"])
        info["targets"] = sorted(target for target, present in (
            ("docs", info["docs"]), ("luals", info["definitions"]),
            ("cpp", path in api.classes and "cpp" in targets),
        ) if present)
    report = coverage(api)
    public = {"format": 1, "api_version": api.version,
              "functions": {f"mirage.sdk.{fn.name}": fn.signature() for fn in functions},
              "types": api.types, "members": api.members}
    config_hash = fingerprint(asdict(policy))
    hashes = {path: fingerprint(asdict(entry)) for path, entry in index.types.items()}
    # The renderers use the current dependency graph every run. Missing dependencies
    # are recorded too, so a newly supplied record changes the dependent fingerprint.
    metadata = {"config_hash": config_hash, "targets": sorted(targets), "package": package, "class": cls,
                "inspection": include_inspection, "types": {
                    path: {"hash": hashes[path], "dependencies": sorted(index.dependencies[path]),
                           "dependency_hash": fingerprint({ref: hashes.get(ref) for ref in index.dependencies[path]})}
                    for path in api.types}}
    files.update({"api-manifest.json": json_text(public), "coverage.json": json_text(report),
                  "diagnostics.json": json_text([asdict(d) for d in
                     sorted(api.diagnostics, key=lambda d: (d.severity, d.code, d.subject, d.message))])})
    return Generation(api, report, public, files, metadata,
                      {"parse": parsed - start, "analyze": analyzed - parsed, "render": perf_counter() - analyzed})
