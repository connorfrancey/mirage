"""Markdown reference for generated APIs and optional SDK inspection."""

from __future__ import annotations

from collections import defaultdict
from html import escape

from .analysis import APIModule, TypeResolver
from .artifacts import digest
from .render import PublicFunction
from .sdk import SDKClass, SDKEnum


def type_page(path: str) -> str:
    stem = path if len(path) <= 160 else f"{path[:100]}-{digest(path.encode())[:20]}"
    return f"docs/type-{stem}.md"


def _type_link(path: str) -> str:
    return type_page(path).partition("/")[2]


def _text(value: str) -> str:
    return escape(value).replace("[", r"\[").replace("]", r"\]").replace("`", r"\`")


def docs(api: APIModule, resolver: TypeResolver, functions: list[PublicFunction],
         include_inspection: bool) -> dict[str, str]:
    files = {}
    title = f"# Mirage scripting API\n\nAPI version `{api.version}`.\n"
    packages: dict[str, list[str]] = defaultdict(list)
    by_owner: dict[str, list[PublicFunction]] = defaultdict(list)
    for fn in functions:
        by_owner[fn.source.rpartition(".")[0]].append(fn)
    paths = api.types.keys() if include_inspection else api.classes.keys()
    for path in paths:
        packages[path.partition(".")[0]].append(path)
    for package, members in sorted(packages.items()):
        title += f"\n- [{package}](package-{package}.md)"
        package_page = [f"# {package}", "", f"API version `{api.version}`.", "", "[Packages](index.md)", ""]
        for path in members:
            info = api.types[path]
            entry = resolver.index.types[path]
            target = type_page(path)
            package_page.append(f"- [{path}]({_type_link(path)})")
            lines = [f"# {path}", "", f"{info['kind'].title()} associated with API version `{api.version}`.", ""]
            if desc := resolver.policy.descriptions.get(path):
                lines.extend((_text(desc), ""))
            if parent := getattr(entry, "super", ""):
                link = f"[`{parent}`]({_type_link(parent)})" if parent in paths else f"`{parent}`"
                lines.extend((f"Inherits from {link}.", ""))
            if path in by_owner:
                lines.extend(("## Runtime functions", ""))
                for fn in by_owner[path]:
                    params = ", ".join(f"`{name}{'?' if optional else ''}` (`{kind}`)"
                                       for name, kind, optional in fn.parameters)
                    returns = ", ".join(f"`{kind}`" + (f" (`{name}`)" if name else "") for name, kind in fn.returns)
                    lines.extend((f"### `mirage.sdk.{fn.name}`", "", f"Source member `{fn.source}`. Game thread only.", "",
                                  f"Parameters {params or 'none'}.", "", f"Returns {returns or 'no values'}.", ""))
                    if fn.deprecated:
                        lines.extend(("Deprecated.", ""))
                    if fn.experimental:
                        lines.extend(("Experimental.", ""))
                    if fn.description:
                        lines.extend((_text(fn.description), ""))
            if include_inspection:
                lines.extend(("## SDK inspection metadata", "", "These layouts describe engine declarations.", ""))
                if isinstance(entry, SDKEnum):
                    lines.extend(f"- `{name}` = {value}" for name, value in entry.values.items())
                else:
                    for prop in entry.properties:
                        kind, _ = resolver.resolve(prop.type)
                        refs = sorted(prop.type.paths() & paths)
                        links = " " + ", ".join(f"[{ref}]({_type_link(ref)})" for ref in refs) if refs else ""
                        array = f" (static array of {prop.type.dimension})" if prop.type.dimension > 1 else ""
                        lines.append(f"- `{prop.name}` — `{kind or prop.type.kind}`{array}.{links}")
                    if isinstance(entry, SDKClass):
                        for fn in entry.functions:
                            signature, _ = resolver.signature(fn)
                            details = ", ".join(f"`{name}` (`{kind}`, {direction}{', optional' if opt else ''})"
                                                for name, kind, direction, opt in signature or ())
                            name = fn.path.rpartition(".")[2]
                            lines.append(f"- `{name}()` — {details or ('no parameters' if signature == () else 'signature unavailable')}.")
            files[target] = "\n".join(lines).rstrip() + "\n"
        files[f"docs/package-{package}.md"] = "\n".join(package_page) + "\n"
    files["docs/index.md"] = title + "\n"
    return files
