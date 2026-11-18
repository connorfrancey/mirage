"""The example Mirage mods (examples/mods): build them the way a mod author would, outside the menu's build.

The native one (hello) is its own CMake project, built against the mod API headers only (api/mirage). With clang-cl
available it is built with clang-cl: a different compiler from the menu's MSVC, which is what the ABI promises to
support. The Lua one (hello-lua) needs no build. `mirage game-test` installs both into the game's mirage/selftest-mods
for the self-test step "Mirage mods" and removes them after.
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from .toolchain import Toolchain

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples" / "mods"
TEST_MODS = ROOT / "tests" / "mods"          # The self-test's own mods (load order, phases, hot reload, a crash)
OUT = ROOT / "build" / "example-mods"

# Level packages the test mods use, copied from the player's own game install when the mods are installed for the
# self-test (the game's content isn't in the repository): (mod folder, path in it, the game's package file)
GAME_PACKAGES = [
    ("layer-test", "levels/LayerTestArea.me1", "Cranes_Bake.me1"),     # A streamed sublevel: 16 static meshes, no Kismet
    ("map-test", "maps/MirageTestMap.me1", "Edge_p.me1"),              # A "new" map: the Prologue's persistent level
]


def clang_cl() -> str | None:
    return shutil.which("clang-cl")


def build_native(tc: Toolchain, name: str = "hello", compiler: str | None = None,
                 source: Path | None = None) -> tuple[Path | None, str]:
    """Builds the native mod in `source` (default examples/mods/<name>) in its own build folder.
    Returns (the built mod's folder, a log tail)."""
    source = source or EXAMPLES / name
    compiler = compiler if compiler is not None else (clang_cl() or "")
    tag = "clang" if compiler and "clang" in Path(compiler).name.lower() else "msvc"
    build = OUT / f"{name}-{tag}"
    configure = [tc.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={tc.ninja}", "-DCMAKE_BUILD_TYPE=Release"]
    if tag == "clang":
        configure += [f"-DCMAKE_CXX_COMPILER={compiler}", "-DCMAKE_CXX_FLAGS=-m32"]
    else:
        configure += ["-DCMAKE_CXX_COMPILER=cl.exe"]
    log = ""
    for cmd in (configure, [tc.cmake, "--build", str(build)]):
        r = subprocess.run(cmd, env=tc.env, capture_output=True, text=True)
        log += r.stdout + r.stderr
        if r.returncode != 0:
            return None, log[-3000:]
    return build / "mod" / name, f"{name} built with {tag}"


def build_csharp(name: str = "hello-cs", source: Path | None = None) -> tuple[Path | None, str]:
    """Builds a C# mod (its .csproj) with the .NET SDK, the way a mod author would, and lays it out as a mod folder:
    mod.toml, the mod's own DLL and PDB (never Mirage.dll: the menu's is shared). Returns (folder, what happened)."""
    source = source or EXAMPLES / name
    dotnet = shutil.which("dotnet")
    if not dotnet:
        return None, "the .NET SDK (dotnet) isn't installed: C# mods can't be built"
    projects = list(source.glob("*.csproj"))
    if len(projects) != 1:
        return None, f"{source} needs exactly one .csproj"
    build = OUT / f"{name}-dotnet"
    r = subprocess.run([dotnet, "build", str(projects[0]), "-c", "Release", "-o", str(build), "--nologo", "-v", "quiet"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stdout + r.stderr)[-3000:]
    folder = OUT / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    shutil.copy2(source / "mod.toml", folder / "mod.toml")
    assembly = projects[0].stem
    for ext in (".dll", ".pdb"):
        f = build / (assembly + ext)
        if f.exists():
            shutil.copy2(f, folder / f.name)
    return folder, f"{name} built with dotnet"


def for_self_test(tc: Toolchain) -> tuple[list[Path] | None, str]:
    """Every mod the self-test loads: the examples and tests/mods, the native ones built. (folders, what was built)"""
    hello, how = build_native(tc, "hello")
    if hello is None:
        return None, how
    native = [hello]
    built = [how]
    for name in ("crasher", "hook-native", "hook-symbol"):
        folder, what = build_native(tc, name, source=TEST_MODS / name)
        if folder is None:
            return None, what
        native.append(folder)
        built.append(what)
    csharp, what = build_csharp("hello-cs")
    if csharp is None:
        return None, what
    native.append(csharp)
    built.append(what)
    lua = [d for d in sorted(TEST_MODS.iterdir()) if d.is_dir() and (d / "main.lua").exists()]
    scripts = [d for d in sorted(TEST_MODS.iterdir()) if d.is_dir() and (d / "main.cs").exists()]  # Compiled in the game
    content = [d for d in sorted(TEST_MODS.iterdir())   # Content only: level layers, maps, routes (step 13), Kismet layers
               if d.is_dir() and any((d / sub).is_dir() for sub in ("content", "levels", "maps", "routes", "kismet"))
               and not (d / "main.lua").exists() and not (d / "main.cs").exists()]
    mods = [*native, EXAMPLES / "hello-lua", EXAMPLES / "high-steps", EXAMPLES / "retexture", EXAMPLES / "higher-jumps", EXAMPLES / "bounce-pad", EXAMPLES / "grapple", *lua,
            EXAMPLES / "hello-script", *scripts, *content]
    return mods, (f"{', '.join(built)}, {len(lua) + 1} Lua mods, {len(scripts) + 1} C# scripts, high-steps and retexture "
                  "(content), bounce-pad (an UnrealScript class)")


def install_for_test(game_dir: Path, folders: list[Path]) -> Path:
    """Copies each mod folder into <game>/mirage/selftest-mods/<folder name> (emptied first)."""
    target = game_dir / "mirage" / "selftest-mods"
    if target.exists():
        shutil.rmtree(target)
    for f in folders:
        shutil.copytree(f, target / f.name, ignore=shutil.ignore_patterns("config.toml"))
    # The game's root (game_dir may be its Binaries folder, where the menu goes)
    maps = next((d / "TdGame" / "CookedPC" / "Maps" for d in (game_dir, game_dir.parent)
                 if (d / "TdGame" / "CookedPC" / "Maps").is_dir()), game_dir / "TdGame" / "CookedPC" / "Maps")
    for mod, rel, package in GAME_PACKAGES:
        if not (target / mod).is_dir():
            continue
        found = next(iter(sorted(maps.rglob(package))), None) if maps.is_dir() else None
        if found is None:
            raise FileNotFoundError(f"{package} isn't in {maps}: the test mod {mod} needs it")
        (target / mod / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(found, target / mod / rel)
    return target


def remove_test_mods(game_dir: Path) -> None:
    target = game_dir / "mirage" / "selftest-mods"
    if target.exists():
        shutil.rmtree(target, ignore_errors=True)
