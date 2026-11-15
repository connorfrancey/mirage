"""Tooling for mod authors (step 3 of the plan's next ten steps): a new mod from a template, and a mod checked, built,
packaged and installed into the game the same way whatever it is written in.

    mirage new my-mod --lang lua|cs|native      a folder with mod.toml and a working module, ready to install
    mirage mod check [folder]                   mod.toml read and checked the way the runtime will
    mirage mod build [folder]                   native: its CMake project (x86); C# project: dotnet; Lua, C# script,
                                                content: nothing to build (checked)
    mirage mod package [folder]                 the built mod zipped: dist/mods/<id>-<version>.zip
    mirage mod install [folder] [--dir game]    the built mod into the game's mirage/mods/<id> (its config.toml kept);
                                                with the game running, a mod with hot_reload = true reloads
    mirage mod test [folder]                    installs it, starts the game and runs its own tests (mirage.test,
                                                mirage::Test, Mod.Test): the self-test step "Mod tests" for it alone
    mirage docs api [--check]                   docs/api-reference.md from loader/include/mirage/api.h
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tomllib
import zipfile
from dataclasses import dataclass
from pathlib import Path

import click
from rich.console import Console

from . import example_mods as em
from .toolchain import ROOT

TEMPLATES = Path(__file__).parent / "templates"
LANGS = {"lua": "Lua: no build, saved files reload in the game",
         "cs": "C# script: compiled in the game, no SDK needed",
         "native": "C++: its own CMake project, built for x86 against the mod API headers"}
ID_RE = re.compile(r"^[a-z][a-z0-9]*(-[a-z0-9]+)*$")
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$")
SETTING_TYPES = {"bool", "int", "float", "string", "choice", "key", "color"}
OUT = ROOT / "build" / "mods"
DIST = ROOT / "dist" / "mods"
KEEP_ON_INSTALL = ("config.toml",)      # What the game wrote into an installed mod's folder (its saved settings)


@dataclass
class ModInfo:
    folder: Path
    id: str
    version: str
    kind: str           # "native", "csproj", "cs", "lua", "content"
    manifest: dict


def _class_name(mod_id: str) -> str:
    return "".join(part.capitalize() for part in mod_id.split("-"))


def _render(text: str, values: dict[str, str]) -> str:
    for k, v in values.items():
        text = text.replace("{{" + k + "}}", v)
    return text


def _author() -> str:
    try:
        r = subprocess.run(["git", "config", "user.name"], capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "Me"


def create(mod_id: str, lang: str, parent: Path, name: str | None = None, description: str | None = None,
           author: str | None = None) -> Path:
    """The mod's folder <parent>/<mod_id> from templates/<lang>. Raises ValueError for a bad id or an existing folder."""
    if not ID_RE.match(mod_id):
        raise ValueError(f"\"{mod_id}\": an id is lowercase letters and digits, words joined by '-' (\"my-mod\")")
    if lang not in LANGS:
        raise ValueError(f"no template for \"{lang}\" ({', '.join(LANGS)})")
    target = parent / mod_id
    if target.exists():
        raise ValueError(f"{target} exists already")
    values = {
        "id": mod_id,
        "name": name or " ".join(part.capitalize() for part in mod_id.split("-")),
        "description": description or "What this mod does",
        "author": author or _author(),
        "class": _class_name(mod_id),
        "ident": mod_id.replace("-", "_"),
        "api_dir": (ROOT / "loader" / "include").as_posix(),
    }
    source = TEMPLATES / lang
    for f in sorted(source.rglob("*")):
        if not f.is_file():
            continue
        rel = Path(_render(str(f.relative_to(source)), values))
        out = target / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(_render(f.read_text(encoding="utf-8"), values), encoding="utf-8", newline="\n")
    return target


def inspect(folder: Path) -> tuple[ModInfo | None, list[str]]:
    """mod.toml read and checked the way the runtime reads it: (the mod, problems). Problems make the runtime refuse it."""
    problems: list[str] = []
    manifest_file = folder / "mod.toml"
    if not manifest_file.is_file():
        return None, [f"{folder} has no mod.toml"]
    try:
        m = tomllib.loads(manifest_file.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        return None, [f"mod.toml: {e}"]
    mod_id = m.get("id", "")
    if not isinstance(mod_id, str) or not ID_RE.match(mod_id):
        problems.append(f"id \"{mod_id}\": lowercase letters and digits, words joined by '-'")
    version = m.get("version", "")
    if not isinstance(version, str) or not SEMVER_RE.match(version):
        problems.append(f"version \"{version}\" isn't a semantic version (\"1.0.0\")")
    if "mirage" not in m:
        problems.append("no mirage = \"^0.1\": which runtime versions it is for")
    for key, s in (m.get("settings") or {}).items():
        t = s.get("type") if isinstance(s, dict) else None
        if t not in SETTING_TYPES:
            problems.append(f"settings.{key}: type \"{t}\" ({', '.join(sorted(SETTING_TYPES))})")
        if t == "choice" and not s.get("options"):
            problems.append(f"settings.{key}: a choice needs options = [...]")

    if (folder / "CMakeLists.txt").is_file():
        kind = "native"
        entry = m.get("entry")
        if not entry:
            problems.append("a native mod needs entry = \"<its>.dll\"")
    elif list(folder.glob("*.csproj")):
        kind = "csproj"
    elif list(folder.glob("*.cs")):
        kind = "cs"
    elif (folder / m.get("lua", "main.lua")).is_file():
        kind = "lua"
    elif any((folder / d).is_dir() for d in ("content", "levels", "maps", "routes")):
        kind = "content"
    else:
        problems.append("nothing to run: no main.lua, no .cs file, no CMakeLists.txt, no content folder")
        kind = "content"
    return ModInfo(folder, mod_id, version, kind, m), problems


def build(info: ModInfo, toolchain) -> tuple[Path | None, str]:
    """The mod as it is installed: (folder, what happened). Native and C# projects are built; the rest is the folder."""
    if info.kind == "native":
        return em.build_native(toolchain, info.id, source=info.folder)
    if info.kind == "csproj":
        return em.build_csharp(info.id, source=info.folder)
    return info.folder, f"{info.id}: {info.kind}, nothing to build"


def _copy_mod(source: Path, target: Path) -> None:
    ignore = shutil.ignore_patterns("build", "bin", "obj", ".vs", ".idea", "*.user", "__pycache__", *KEEP_ON_INSTALL)
    shutil.copytree(source, target, ignore=ignore, dirs_exist_ok=True)


def install(built: Path, mod_id: str, game_dir: Path) -> Path:
    """Copies the built mod into <game>/mirage/mods/<id>: what was there goes, except the settings the game saved."""
    target = game_dir / "mirage" / "mods" / mod_id
    kept: dict[str, bytes] = {}
    if target.exists():
        for name in KEEP_ON_INSTALL:
            if (target / name).is_file():
                kept[name] = (target / name).read_bytes()
        for child in target.iterdir():
            if child.name.startswith("."):      # The runtime's own (.mirage: the running copy of a native DLL)
                continue
            shutil.rmtree(child) if child.is_dir() else child.unlink()
    target.mkdir(parents=True, exist_ok=True)
    _copy_mod(built, target)
    for name, data in kept.items():
        (target / name).write_bytes(data)
    return target


def package(built: Path, info: ModInfo) -> Path:
    DIST.mkdir(parents=True, exist_ok=True)
    out = DIST / f"{info.id}-{info.version}.zip"
    staging = OUT / "package" / info.id
    if staging.exists():
        shutil.rmtree(staging)
    _copy_mod(built, staging)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(staging.rglob("*")):
            if f.is_file():
                z.write(f, Path(info.id) / f.relative_to(staging))
    return out


def register(cli: click.Group, console: Console, toolchain_of, find_game_dir) -> None:
    """`mirage new`, `mirage mod ...` and `mirage docs ...` on the CLI. toolchain_of(ctx): the x86 toolchain;
    find_game_dir(): the game's Binaries folder or None."""

    def resolve(ctx: click.Context, folder: Path) -> ModInfo:
        info, problems = inspect(folder.resolve())
        for p in problems:
            console.print(f"[red]{p}[/]")
        if info is None or problems:
            ctx.exit(1)
        return info

    def built_or_exit(ctx: click.Context, info: ModInfo) -> Path:
        tc = toolchain_of(ctx) if info.kind == "native" else None
        with console.status(f"Building {info.id} ({info.kind})"):
            folder, how = build(info, tc)
        if folder is None:
            console.print(f"[red]{info.id} didn't build:[/]\n{how}")
            ctx.exit(1)
        console.print(how)
        return folder

    @cli.command("new")
    @click.argument("mod_id")
    @click.option("--lang", type=click.Choice(sorted(LANGS)), default="lua", show_default=True,
                  help="; ".join(f"{k}: {v}" for k, v in LANGS.items()))
    @click.option("--dir", "parent", type=click.Path(path_type=Path), default=Path("."), show_default=True,
                  help="Where the mod's folder goes.")
    @click.option("--name", help="The name the mod manager shows (default: from the id).")
    @click.option("--description", help="One line on what it does.")
    @click.pass_context
    def new(ctx: click.Context, mod_id: str, lang: str, parent: Path, name: str | None, description: str | None) -> None:
        """A new mod from a template: mod.toml and one working module. Then: mirage mod install <folder>."""
        try:
            folder = create(mod_id, lang, parent.resolve(), name, description)
        except ValueError as e:
            console.print(f"[red]{e}[/]")
            ctx.exit(1)
        console.print(f"Made [cyan]{folder}[/] ({LANGS[lang]}).")
        console.print(f"Next: [bold]mirage mod install {folder}[/], start the game, and find \"{name or mod_id}\" in the Mods "
                      "column. docs/getting-started.md has the rest.")

    @cli.group("mod")
    def mod() -> None:
        """Check, build, package and install a mod (any kind: Lua, C# script or project, native, content)."""

    folder_arg = click.argument("folder", type=click.Path(path_type=Path, exists=True, file_okay=False), default=".")

    @mod.command("check")
    @folder_arg
    @click.pass_context
    def check(ctx: click.Context, folder: Path) -> None:
        """mod.toml read and checked the way the runtime will (id, version, settings, what it runs)."""
        info = resolve(ctx, folder)
        console.print(f"[green]{info.id} {info.version}[/]: {info.kind}, mod.toml is fine")

    @mod.command("build")
    @folder_arg
    @click.pass_context
    def build_cmd(ctx: click.Context, folder: Path) -> None:
        """Builds it (native: its CMake project for x86; C# project: dotnet). Lua, scripts and content: checked only."""
        info = resolve(ctx, folder)
        out = built_or_exit(ctx, info)
        console.print(f"[green]Built[/]: {out}")

    @mod.command("package")
    @folder_arg
    @click.pass_context
    def package_cmd(ctx: click.Context, folder: Path) -> None:
        """Builds it and zips the mod as it is installed: dist/mods/<id>-<version>.zip."""
        info = resolve(ctx, folder)
        out = package(built_or_exit(ctx, info), info)
        console.print(f"[green]Packaged[/]: {out}")

    @mod.command("install")
    @folder_arg
    @click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="The game's Binaries folder (default: found).")
    @click.pass_context
    def install_cmd(ctx: click.Context, folder: Path, game_dir: Path | None) -> None:
        """Builds it and copies it into the game's mirage/mods/<id> (the settings the game saved there are kept)."""
        info = resolve(ctx, folder)
        game_dir = game_dir or find_game_dir()
        if game_dir is None or not (game_dir / "mirage").is_dir():
            console.print("[red]No Mirage install found[/] (Binaries\\mirage): install Mirage first, or pass --dir.")
            ctx.exit(1)
        target = install(built_or_exit(ctx, info), info.id, game_dir)
        reload = info.manifest.get("hot_reload", False) or info.kind in ("lua", "cs")
        console.print(f"[green]Installed[/] {info.id} {info.version} into {target}." +
                      (" With the game running it reloads now." if reload else " It loads at the next start."))

    @mod.command("test")
    @folder_arg
    @click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="The game's Binaries folder (default: found).")
    @click.option("--visible", is_flag=True, help="Show the game while it runs (default: minimized).")
    @click.pass_context
    def test_cmd(ctx: click.Context, folder: Path, game_dir: Path | None, visible: bool) -> None:
        """Installs it, starts the game (the build Mirage was last built from) and runs its tests: the step "Mod tests"
        with only this mod's. The game starts and closes by itself."""
        info = resolve(ctx, folder)
        game_dir = game_dir or find_game_dir()
        if game_dir is None or not (game_dir / "mirage").is_dir():
            console.print("[red]No Mirage install found[/] (Binaries\mirage): install Mirage first, or pass --dir.")
            ctx.exit(1)
        install(built_or_exit(ctx, info), info.id, game_dir)
        console.print(f"Testing {info.id} in the game (it starts and closes by itself)...")
        game_test = ctx.find_root().command.commands["game-test"]
        ctx.invoke(game_test, no_build=True, game_dir=game_dir, only="Mod tests", mod_tests=info.id, minimized=not visible)

    @cli.group("docs")
    def docs() -> None:
        """Generated documentation."""

    @docs.command("api")
    @click.option("--check", is_flag=True, help="Exit 1 when docs/api-reference.md is out of date (no writing).")
    def docs_api(check: bool) -> None:
        """docs/api-reference.md from loader/include/mirage/api.h: every function and type, with its comment."""
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_api_docs.py")] + (["--check"] if check else []))
        sys.exit(r.returncode)
