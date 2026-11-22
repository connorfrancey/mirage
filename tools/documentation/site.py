"""Stage the Mirage guides, API pages and Doxygen assets before a strict MkDocs build."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[2]
GUIDES = (
    "getting-started.md", "infrastructure.md", "memory.md", "mod-authoring-ai.md", "mod-classes.md", "models.md",
    "csharp-mods.md",
    "documentation/cpp.md", "documentation/lua.md", "documentation/sdk.md", "documentation/README.md",
)


def write(root: Path, name: str, content: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def stage_lua(content: Path, args: argparse.Namespace) -> None:
    if args.no_lua:
        return
    from mirage_apigen import DEFAULT_CONFIG, DEFAULT_INPUT
    from mirage_apigen.pipeline import generate

    result = generate(args.lua_input or DEFAULT_INPUT, args.lua_config or DEFAULT_CONFIG, ("docs",),
                      include_inspection=args.lua_inspection)
    if errors := [d for d in result.api.diagnostics if d.severity == "error"]:
        raise ValueError("\n".join(f"{d.subject}: {d.message}" for d in errors))
    for name, data in result.files.items():
        if name.startswith("docs/"):
            write(content, "lua-reference/" + name.removeprefix("docs/"), data)
    with (content / "documentation/lua.md").open("a", encoding="utf-8", newline="\n") as page:
        page.write("\n## Generated reference\n\n[MirageAPIGen reference](../lua-reference/index.md)\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--doxygen", type=Path, required=True)
    parser.add_argument("--coverage", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--no-lua", action="store_true")
    parser.add_argument("--lua-inspection", action="store_true")
    parser.add_argument("--lua-input", type=Path)
    parser.add_argument("--lua-config", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    doxygen = args.doxygen.resolve()
    if not (doxygen / "index.html").is_file():
        parser.error(f"Missing Doxygen HTML in {doxygen}")

    sys.path.insert(0, str(ROOT / "tools"))
    from gen_api_docs import render

    try:
        with TemporaryDirectory(prefix="mkdocs-", dir=output) as temp:
            content = Path(temp)
            for name in GUIDES:
                target = content / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / "docs" / name, target)
            shutil.copyfile(ROOT / "docs/docs-index.md", content / "index.md")
            write(content, "api-reference.md", render())
            shutil.copytree(doxygen, content / "cpp-reference")
            if not (doxygen / "topics.html").is_file() and (doxygen / "modules.html").is_file():
                cpp = content / "documentation/cpp.md"
                write(content, "documentation/cpp.md", cpp.read_text(encoding="utf-8").replace("topics.html", "modules.html"))
            if args.coverage is not None:
                shutil.copyfile(args.coverage, content / "cpp-coverage.json")
                with (content / "documentation/cpp.md").open("a", encoding="utf-8", newline="\n") as page:
                    page.write("\n[Documentation coverage report](../cpp-coverage.json)\n")
            stage_lua(content, args)
            env = {**os.environ, "MIRAGE_DOCS_CONTENT": str(content), "MIRAGE_DOCS_SITE": str(output / "site"),
                   "MIRAGE_DOCS_SITE_NAME": f"Mirage {args.version}"}
            env.setdefault("SOURCE_DATE_EPOCH", "0")
            return subprocess.run([sys.executable, "-m", "mkdocs", "build", "--strict", "-f", str(ROOT / "mkdocs.yml")],
                                  cwd=ROOT, env=env, check=False).returncode
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Documentation website failed: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
