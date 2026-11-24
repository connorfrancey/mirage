"""`mirage`: one command for building, testing, installing and packaging the project.

(or `python mirage.py ...` / `uv run mirage.py ...` from the repository root)

    uv run mirage                       interactive menu (everything below, picked from a list)
    uv run mirage build                 debug build of Mirage.dll (configures first if needed)
    uv run mirage build -c release      release build
    uv run mirage build --with tests    also build an optional part (tests, benchmarks, installer, launcher)
    uv run mirage build --disable all   menu GUI only, no features (see `mirage features`)
    uv run mirage build --only levels   only the Levels features (also: --enable / --disable NAME)
    uv run mirage features              list the features and which ones the build folder builds
    uv run mirage dev-test              build, start the game with the menu injected, report crashes
    uv run mirage minimal               only the D3D9 hook + ImGui demo window (INSERT), nothing else
    uv run mirage errors                build everything, list every error grouped by file
    uv run mirage test                  build and run the unit tests
    uv run mirage bench                 build (release) and run the benchmarks
    uv run mirage install-game          build, then install into the game (UAC prompt)
    uv run mirage package               release installer -> dist/
    uv run mirage verbose-build         build stage by stage, test each one without the game, report failures
    uv run mirage --copy-info           copy diagnostic info (logs, builds, game, crashes) to the clipboard

Developer mode (`mirage --dev ...` or MIRAGE_DEV=1): full tracebacks, timings, `mirage dev ...` tools (see
devmode.py), and builds with MIRAGE_DEV_MODE: the DLL switches its stored developer mode on, which shows the Test
module (in-game self-tests with an output console). `--no-dev` builds without it (the menu keeps what it stored).
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from datetime import datetime
from pathlib import Path

import click
from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.table import Table
from mirage_apigen.cli import api as api_commands

from . import build as b
from . import example_mods as em
from . import features as feat
from .game import INSTALLER, LAUNCHER, detected_installs, is_elevated, project_version, run_elevated
from .tool_logging import LoggedGroup
from .toolchain import ROOT, Toolchain, ToolchainError, cmake_version, find_toolchain

console = Console(highlight=False)
FILES_ZIP = "mirage-files.zip"   # The DLLs unpacked, for install.sh (see `package`)
DIST_FILES = Path(__file__).parent / "dist"     # README.txt and license texts for the release zip

# Everything compiled into the menu that isn't ours, for THIRD-PARTY-NOTICES.txt in the release zip:
# (name, what it's for, where it's from, CMake package name, license files in its sources, the first that exists wins)
THIRD_PARTY = [
    ("Dear ImGui", "the menu's user interface", "https://github.com/ocornut/imgui", "imgui", ["LICENSE.txt"]),
    # MirageHook (ours) hooks the game's functions; these are what it compiles in
    ("Zydis", "decoding the hooked functions' instructions", "https://github.com/zyantific/zydis", "zydis", ["LICENSE"]),
    ("Zycore (bundled with Zydis)", "Zydis's support library", "https://github.com/zyantific/zycore-c", "zycore", ["LICENSE"]),
    ("AsmJit", "generating mid-function hook stubs", "https://github.com/asmjit/asmjit", "asmjit", ["LICENSE.md"]),
    ("Highway", "fast signature scanning (SIMD)", "https://github.com/google/highway", "highway", ["LICENSE"]),
    ("concurrentqueue", "the hooking library's diagnostic events", "https://github.com/cameron314/concurrentqueue",
     "concurrentqueue", ["LICENSE.md"]),
    ("readerwriterqueue", "the hooking library's diagnostic events", "https://github.com/cameron314/readerwriterqueue",
     "readerwriterqueue", ["LICENSE.md"]),
    ("spdlog", "the log", "https://github.com/gabime/spdlog", "spdlog", ["LICENSE"]),
    ("{fmt}", "text formatting", "https://github.com/fmtlib/fmt", "fmt", ["LICENSE"]),
    ("UnrealLib", "reading and writing Unreal Engine packages", "https://github.com/connorfrancey/UnrealLib",
     "unreallib", ["LICENSE"]),
    ("zlib", "decompressing Unreal Engine packages", "https://github.com/madler/zlib", "zlib", ["LICENSE"]),
    ("Zstandard", "compressing old logs", "https://github.com/facebook/zstd", "zstd", ["LICENSE"]),
    ("Glaze", "settings, caches and every JSON file", "https://github.com/stephenberry/glaze", "glaze",
     ["LICENSE"]),
    ("imgui-filebrowser", "the file browser", "https://github.com/AirGuanZ/imgui-filebrowser", "imgui_filebrowser",
     ["LICENSE", "LICENSE.txt"]),
]

config_option = click.option(
    "-c", "--config", type=click.Choice(sorted(b.CONFIGS)), default="debug", show_default=True,
    help="Build configuration.",
)
verbose_option = click.option("-v", "--verbose", is_flag=True, help="Print every line of the tool output.")


def _toolchain(ctx: click.Context) -> Toolchain:
    obj = ctx.ensure_object(dict)
    if "toolchain" not in obj:
        try:
            with console.status("Setting up the x86 MSVC environment"):
                obj["toolchain"] = find_toolchain(refresh=obj.get("refresh", False))
        except ToolchainError as e:
            console.print(f"[bold red]Toolchain problem:[/] {e}")
            ctx.exit(2)
    return obj["toolchain"]


def _ensure_configured(ctx: click.Context, tc: Toolchain, config: str, verbose: bool) -> None:
    if b.is_configured(config):
        return
    result = b.configure(console, tc, config, verbose=verbose)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


def _enable_parts(ctx: click.Context, config: str, parts: tuple[str, ...] | list[str], verbose: bool = False) -> None:
    """Turn on optional parts (tests/installer/launcher) in a build folder, reconfiguring only if one is off.
    The setting stays in the CMake cache, so later plain builds keep building it."""
    missing = [b.OPTIONS[p] for p in parts if not b.option_enabled(config, b.OPTIONS[p])]
    if not missing:
        return
    console.print(f"Enabling {', '.join(missing)} in {b.build_dir(config).relative_to(ROOT)}")
    result = b.configure(console, _toolchain(ctx), config, verbose=verbose, defines=tuple(f"{m}=ON" for m in missing))
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


with_option = click.option(
    "--with", "parts", multiple=True, type=click.Choice(sorted(b.OPTIONS)),
    help="Also build an optional part (repeatable). Only Mirage.dll is built by default.",
)


def feature_options(f):
    """--only / --enable / --disable NAME, where NAME is a feature, a category or `all`. Remembered by the build folder."""
    help_name = "a feature, a category or 'all' (see `mirage features`)"
    f = click.option("--disable", "disable", multiple=True, metavar="NAME",
                     help=f"Leave out {help_name} (repeatable).")(f)
    f = click.option("--enable", "enable", multiple=True, metavar="NAME",
                     help=f"Build {help_name} (repeatable).")(f)
    f = click.option("--default-features", "reset_features", is_flag=True,
                     help="Back to the defaults in cmake/Features.cmake (before any --only/--enable/--disable).")(f)
    f = click.option("--only", "only", multiple=True, metavar="NAME",
                     help=f"Build only these: {help_name} (repeatable).")(f)
    return f


def _feature_defines(ctx: click.Context, config: str, only: tuple[str, ...], enable: tuple[str, ...],
                     disable: tuple[str, ...], reset: bool = False) -> tuple[str, ...]:
    """The -D values the feature toggles need in this build folder (empty when there are no toggles or nothing changes)."""
    if not (only or enable or disable or reset):
        return ()
    features = feat.load()
    try:
        state = feat.apply(features, feat.current_state(config, features), only, enable, disable, reset)
    except feat.UnknownFeature as e:
        known = ", ".join([feat.ALL, *feat.categories(features)])
        raise click.BadParameter(f"unknown feature or category '{e}' (categories: {known}; "
                                 "`mirage features` lists the features)", ctx=ctx,
                                 param_hint="--only/--enable/--disable")
    except ValueError as e:
        raise click.BadParameter(str(e), ctx=ctx, param_hint="--only/--enable/--disable")
    defines = feat.defines(config, features, state)
    console.print(f"Features: {feat.summary(features, state)}" + ("" if defines else " [dim](unchanged)[/]"))
    return defines


OPTIMIZATIONS = "MIRAGE_OPTIMIZATIONS"

optimizations_option = click.option(
    "--optimizations/--no-optimizations", "optimizations", default=None,
    help="Build the performance optimizations (caches, scheduler intervals and budget) or leave them out: without "
         "them everything works but does the work every time. Remembered by the build folder (default: on).")


def optimizations_on(config: str) -> bool:
    """Whether the build folder builds them (they default to on, also when it is not configured yet)."""
    return b.cache_value(config, OPTIMIZATIONS) is None or b.option_enabled(config, OPTIMIZATIONS)


def _optimization_defines(config: str, optimizations: bool | None) -> tuple[str, ...]:
    """The -D value --[no-]optimizations needs in this build folder (empty when not given or unchanged)."""
    if optimizations is None:
        return ()
    if b.cache_value(config, OPTIMIZATIONS) is not None and optimizations_on(config) == optimizations:
        return ()
    return (f"{OPTIMIZATIONS}={'ON' if optimizations else 'OFF'}",)


TRACY = "MIRAGE_TRACY"

tracy_option = click.option(
    "--tracy/--no-tracy", "tracy", default=None,
    help="Build with the Tracy profiler client: connect the Tracy profiler (the version in cmake/Versions.cmake) to "
         "localhost while the game runs. Remembered by the build folder (default: off).")


def tracy_on(config: str) -> bool:
    return b.option_enabled(config, TRACY)


def _tracy_defines(config: str, tracy: bool | None) -> tuple[str, ...]:
    """The -D value --[no-]tracy needs in this build folder (empty when not given or unchanged)."""
    if tracy is None or (b.cache_value(config, TRACY) is not None and tracy_on(config) == tracy):
        return ()
    return (f"{TRACY}={'ON' if tracy else 'OFF'}",)


def _apply_features(ctx: click.Context, config: str, only: tuple[str, ...], enable: tuple[str, ...],
                    disable: tuple[str, ...], reset: bool, verbose: bool, extra: tuple[str, ...] = ()) -> None:
    defines = _feature_defines(ctx, config, only, enable, disable, reset) + extra
    if not defines:
        return
    result = b.configure(console, _toolchain(ctx), config, verbose=verbose, defines=defines)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


def _dev_wanted(ctx: click.Context) -> bool | None:
    """MIRAGE_DEV_MODE for this build: --dev / --no-dev, else ON in developer mode (MIRAGE_DEV=1), else unchanged."""
    flag = ctx.find_root().ensure_object(dict).get("dev")
    if flag is not None:
        return flag
    return True if _devmode.enabled() else None


def _apply_dev_mode(ctx: click.Context, tc: Toolchain, config: str, verbose: bool) -> None:
    want = _dev_wanted(ctx)
    if want is None or b.option_enabled(config, "MIRAGE_DEV_MODE") == want:
        return
    console.print(f"{'Enabling' if want else 'Disabling'} developer mode (MIRAGE_DEV_MODE) in "
                  f"{b.build_dir(config).relative_to(ROOT)}")
    result = b.configure(console, tc, config, verbose=verbose, defines=(f"MIRAGE_DEV_MODE={'ON' if want else 'OFF'}",))
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)


def _build(ctx: click.Context, config: str, targets: tuple[str, ...] = (), keep_going: bool = False,
           jobs: int | None = None, verbose: bool = False, warnings: str = "ours") -> b.RunResult:
    tc = _toolchain(ctx)
    _ensure_configured(ctx, tc, config, verbose)
    _apply_dev_mode(ctx, tc, config, verbose)
    start = time.perf_counter()
    result = b.build(console, tc, config, targets, keep_going, jobs, verbose, warnings)
    took = time.perf_counter() - start
    ours = [w for w in result.warnings if w.ours]
    if result.returncode == 0:
        note = "" if optimizations_on(config) else ", [yellow]no optimizations[/]"
        if tracy_on(config):
            note += ", [cyan]Tracy[/]"
        if b.option_enabled(config, "MIRAGE_DEV_MODE"):
            note += ", [magenta]developer mode[/]"
        console.print(f"[bold green]Build succeeded[/] ({config}, {took:.1f}s, {len(ours)} warning(s) in our code{note})")
    else:
        console.print(f"[bold red]Build failed[/] ({config}, {took:.1f}s, {len(result.errors)} error(s)). "
                      f"Full log: [cyan]{result.log}[/]")
        _explain_locked_outputs(config, result)
    return result


def _explain_locked_outputs(config: str, result: b.RunResult) -> None:
    """LNK1168 (the linker can't overwrite its output): say which program has the file open."""
    from .locks import holders

    for d in result.errors:
        if d.code != "LNK1168":
            continue
        m = re.search(r"cannot open (.+?) for writing", d.message)
        if not m:
            continue
        path = Path(m[1].strip())
        if not path.is_absolute():
            path = b.build_dir(config) / path
        who = holders(path)
        if who:
            names = ", ".join(f"{name} (PID {pid})" for pid, name in who)
            console.print(f"[yellow]{path.name} is open in {names}[/]: the linker can't replace it while it is. "
                          "Close it there (an IDE: the file's tab, or a debug / attach session; the game: quit it, "
                          "or press END in it to unload the menu), then build again.")
        else:
            console.print(f"[yellow]Something had {path.name} open[/] (it may have closed it by now): build again.")


def built_dll(config: str) -> tuple[Path, Path | None]:
    """(the DLL in the build folder, its copy from MIRAGE_COPY_TO_BUILD_DIR or None when there is no copy)."""
    dll = _artifact(config, "Mirage.dll")
    copy_dir = b.cache_value(config, "MIRAGE_BUILD_OUTPUT_DIR")
    copy = None
    if copy_dir and (b.cache_value(config, "MIRAGE_COPY_TO_BUILD_DIR") is None
                     or b.option_enabled(config, "MIRAGE_COPY_TO_BUILD_DIR")):
        copy = Path(copy_dir) / "Mirage.dll"
    return dll, copy


def _report_dll(config: str) -> None:
    """Where the DLL that was just built is: the copy in build/ (the one to use) and the original in the build folder."""
    dll, copy = built_dll(config)
    if not dll.exists():
        console.print(f"[yellow]Mirage.dll not found in {b.build_dir(config)}[/]")
        return
    size = f"{dll.stat().st_size / (1024 * 1024):.1f} MB"
    if copy and copy.exists() and copy.stat().st_size == dll.stat().st_size \
            and copy.stat().st_mtime >= dll.stat().st_mtime - 2:
        console.print(f"[bold]DLL:[/] [cyan]{copy.resolve()}[/] ({size})")
        console.print(f"  [dim]built in {dll.resolve()}[/]")
    else:
        console.print(f"[bold]DLL:[/] [cyan]{dll.resolve()}[/] ({size})")
        if copy:
            console.print(f"  [yellow]the copy in {copy.parent} is not this build[/]")


def _artifact(config: str, name: str) -> Path:
    return b.build_dir(config) / name


# ---- commands ------------------------------------------------------------------------------------------------------
@click.group(cls=LoggedGroup, context_settings={"help_option_names": ["-h", "--help"]}, invoke_without_command=True)
@click.option("--refresh-env", is_flag=True, help="Capture the MSVC environment again (after a Visual Studio update).")
@click.option("--dev/--no-dev", "dev", default=None,
              help="Developer mode: builds switch the menu's developer mode on (the Test module: in-game self-tests), "
                   "plus the tool's developer tools (like MIRAGE_DEV=1). --no-dev builds without it.")
@click.option("--copy-info", is_flag=True,
              help="Copy everything needed to diagnose a problem (system, git, build folders, game install, crash "
                   "records, the menu's current and archived logs) to the clipboard, and exit.")
@click.pass_context
def cli(ctx: click.Context, refresh_env: bool, dev: bool | None, copy_info: bool) -> None:
    """Build, test, install and package Mirage.

    Run it without a command for an interactive menu.
    """
    ctx.ensure_object(dict)["refresh"] = refresh_env
    ctx.obj["dev"] = dev
    if copy_info:
        _copy_info()
        ctx.exit(0)
    if ctx.invoked_subcommand is None:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            click.echo(ctx.get_help())      # Piped / CI: no menu to answer
            return
        try:
            from .interactive import run
        except ImportError as e:
            if e.name not in ("questionary", "prompt_toolkit"):
                raise
            console.print(f"[red]The interactive menu needs questionary ({e.name} is missing).[/] Run it with "
                          "`uv run mirage.py`, or: pip install questionary. Commands work without it (--help).")
            ctx.exit(2)
        run(cli, console)


def _copy_info() -> None:
    from .diaginfo import collect, copy_to_clipboard

    with console.status("Collecting diagnostic info"):
        report = collect()
    saved = ROOT / "build" / "diag-info.txt"
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_text(report, encoding="utf-8")
    try:
        copy_to_clipboard(report)
        console.print(f"[bold green]Diagnostic info copied to the clipboard[/] ({len(report.splitlines())} lines, "
                      f"{len(report) // 1024} KB). Also saved to {saved.relative_to(ROOT)}.")
    except OSError as e:
        console.print(f"[yellow]Could not use the clipboard ({e}).[/] The info is in {saved}.")
    console.print("[dim]It contains file paths (with your user name) and git info: check it before posting publicly.[/]")


@cli.command()
@click.pass_context
def info(ctx: click.Context) -> None:
    """Show the toolchain, build folders and the Mirror's Edge install that was found."""
    tc = _toolchain(ctx)
    table = Table(show_header=False, box=None, pad_edge=False)
    table.add_column(style="bold")
    table.add_column()
    table.add_row("Project version", project_version())
    table.add_row("Visual Studio" if tc.vs_path else "Compiler", str(tc.vs_path) if tc.vs_path else tc.compiler)
    table.add_row("MSVC", tc.env.get("VCToolsVersion", "?"))
    table.add_row("CMake", f"{tc.cmake} ({'.'.join(map(str, cmake_version(tc.cmake) or ()))})")
    table.add_row("Ninja", tc.ninja)
    for config in sorted(b.CONFIGS):
        folder = b.build_dir(config)
        state = "configured" if b.is_configured(config) else "not configured"
        installer = _artifact(config, INSTALLER)
        if installer.exists():
            state += f", installer built {time.strftime('%Y-%m-%d %H:%M', time.localtime(installer.stat().st_mtime))}"
        table.add_row(f"Build ({config})", f"{folder.relative_to(ROOT)} [dim]({state})[/]")

    launcher = next((p for p in (_artifact(c, LAUNCHER) for c in ("debug", "release")) if p.exists()), None)
    found = detected_installs(launcher) if launcher else "(build the launcher to detect the game)"
    table.add_row("Game", found or "not found")
    console.print(Panel(table, title="Mirage", expand=False))


@cli.command()
@click.option("--check-tag", metavar="TAG", help="Fail unless TAG (e.g. v0.1.0 or refs/tags/v0.1.0) is this version.")
def version(check_tag: str | None) -> None:
    """Print the project version (MIRAGE_VERSION in CMakeLists.txt)."""
    current = project_version()
    if check_tag:
        tag = check_tag.removeprefix("refs/tags/").removeprefix("v")
        if tag != current:
            console.print(f"[bold red]Tag {check_tag} does not match MIRAGE_VERSION {current}[/] "
                          "(update set(MIRAGE_VERSION ...) in CMakeLists.txt, or tag v" + current + ")")
            sys.exit(1)
    click.echo(current)


@cli.command()
@config_option
@click.option("--fresh", is_flag=True, help="Configure from scratch (cmake --fresh).")
@click.option("-D", "defines", multiple=True, metavar="VAR=VALUE", help="CMake cache variable (repeatable).")
@with_option
@feature_options
@optimizations_option
@tracy_option
@verbose_option
@click.pass_context
def configure(ctx: click.Context, config: str, fresh: bool, defines: tuple[str, ...], parts: tuple[str, ...],
              only: tuple[str, ...], enable: tuple[str, ...], disable: tuple[str, ...], reset_features: bool,
              optimizations: bool | None, tracy: bool | None, verbose: bool) -> None:
    """Run CMake for a configuration."""
    if optimizations is not None:
        defines += (f"{OPTIMIZATIONS}={'ON' if optimizations else 'OFF'}",)
    if tracy is not None:
        defines += (f"{TRACY}={'ON' if tracy else 'OFF'}",)
    defines = (_feature_defines(ctx, config, only, enable, disable, reset_features) + defines
               + tuple(f"{b.OPTIONS[p]}=ON" for p in parts))
    result = b.configure(console, _toolchain(ctx), config, fresh, verbose, defines)
    if result.returncode != 0:
        console.print(f"[bold red]Configure failed[/] (log: {result.log})")
        ctx.exit(1)
    console.print(f"[bold green]Configured[/] {b.build_dir(config).relative_to(ROOT)}")


@cli.command("build")
@config_option
@click.option("-t", "--target", "targets", multiple=True, help="Build only this target (repeatable).")
@click.option("--clean", "clean_first", is_flag=True, help="Delete the build folder first (full rebuild).")
@click.option("-k", "--keep-going", is_flag=True, help="Don't stop at the first failing file.")
@click.option("-j", "--jobs", type=int, help="Parallel jobs (default: Ninja's choice).")
@click.option("-w", "--warnings", type=click.Choice(["ours", "all", "none"]), default="ours", show_default=True,
              help="Which warnings to print.")
@with_option
@feature_options
@optimizations_option
@tracy_option
@verbose_option
@click.pass_context
def build_cmd(ctx: click.Context, config: str, targets: tuple[str, ...], clean_first: bool, keep_going: bool,
              jobs: int | None, warnings: str, parts: tuple[str, ...], only: tuple[str, ...],
              enable: tuple[str, ...], disable: tuple[str, ...], reset_features: bool, optimizations: bool | None,
              tracy: bool | None, verbose: bool) -> None:
    """Build (configures first when needed). By default only Mirage.dll; see --with.

    
    Feature toggles are remembered by the build folder, so later plain builds keep them:
      mirage build --disable all                    menu GUI only, no in-game functionality
      mirage build --only levels --enable no_clip   the Levels tab and No Clip
      mirage build --enable debug --disable log_viewer
      mirage build --enable all                     everything, TAS included
      mirage build --default-features               back to the defaults (everything but TAS)
      mirage build --no-optimizations               no caches / scheduler intervals (--optimizations: back on)
      mirage build --tracy                          with the Tracy profiler client (--no-tracy: without)
    """
    if clean_first:
        _remove_build_dir(config)
    _apply_features(ctx, config, only, enable, disable, reset_features, verbose,
                    extra=_optimization_defines(config, optimizations) + _tracy_defines(config, tracy))
    _enable_parts(ctx, config, parts, verbose)
    result = _build(ctx, config, targets, keep_going, jobs, verbose, warnings)
    if result.returncode == 0:
        _report_dll(config)
        for part, name in (("installer", INSTALLER), ("launcher", LAUNCHER)):   # Only parts this folder builds
            if b.option_enabled(config, b.OPTIONS[part]) and (p := _artifact(config, name)).exists():
                console.print(f"  {part}: [dim]{p}[/]")
    ctx.exit(result.returncode and 1)


@cli.command()
@config_option
@click.option("--clean", "clean_first", is_flag=True, help="Delete the minimal build folder first.")
@click.option("-s", "--stage", default=None, metavar="N|last",
              help="How far to go: 0 = bare hook ... last = every feature (remembered by the build folder; "
                   "--list shows the stages).")
@click.option("--next", "next_stage", is_flag=True, help="Go one stage further than the last build.")
@click.option("-l", "--list", "list_stages", is_flag=True, help="List the stages and what each adds, then exit.")
@click.option("-w", "--warnings", type=click.Choice(["ours", "all", "none"]), default="ours", show_default=True,
              help="Which warnings to print.")
@optimizations_option
@verbose_option
@click.pass_context
def minimal(ctx: click.Context, config: str, clean_first: bool, stage: str | None, next_stage: bool,
            list_stages: bool, warnings: str, optimizations: bool | None, verbose: bool) -> None:
    """Staged build: start from only what an injected overlay strictly needs, then add the trainer back one stage at a
    time, least likely to break the game first.

    \b
    Stage 0 is the D3D9 hook and ImGui's demo window (INSERT toggles it, END unloads), linking only MirageHook, ImGui
    and d3d9. Each stage adds a layer or a group of features; the last one is the whole trainer, every feature on.
    The stages and each feature's stage are in cmake/Features.cmake.
      mirage minimal --list        what each stage adds
      mirage minimal --stage 4     build up to stage 4
      mirage minimal --next        one stage further than last time

    Uses its own build folders (cmake-build-x86-minimal-<config>) and copies the DLL to build/minimal/, so the full
    build is left alone. Inject it with: mirage_launcher --dll build\\minimal\\Mirage.dll
    """
    all_stages = feat.stages()
    features = feat.load()
    last = all_stages[-1][0]
    folder = b.MINIMAL[config]
    cached = b.cache_value(folder, "MIRAGE_MINIMAL_STAGE")
    current = int(cached) if cached and cached.isdigit() else None

    if list_stages:
        console.print(_stage_table(all_stages, features, current))
        return
    if stage is not None and next_stage:
        raise click.UsageError("use --stage or --next, not both")
    if next_stage:
        if current is None:
            stage = "0"
        elif current >= last:
            console.print(f"Already at the last stage ({last}): every feature is on.")
            stage = str(last)
        else:
            stage = str(current + 1)
    if stage is not None:
        stage = str(last) if stage.lower() == "last" else stage
        if not stage.isdigit() or int(stage) > last:
            raise click.BadParameter(f"must be 0-{last} or 'last'", ctx=ctx, param_hint="--stage")

    if clean_first:
        _remove_build_dir(folder)
        cached = None
    defines = _optimization_defines(folder, optimizations)
    if stage is not None and cached != stage:
        defines += (f"MIRAGE_MINIMAL_STAGE={stage}",)
    if defines:
        result = b.configure(console, _toolchain(ctx), folder, verbose=verbose, defines=defines)
        if result.returncode != 0:
            console.print(f"[bold red]Configure failed[/] (log: {result.log})")
            ctx.exit(1)
    shown = int(stage) if stage is not None else (current or 0)
    added = dict(all_stages).get(shown, "?")
    new = [f.name for f in features if f.stage == shown]
    console.print(f"[bold]Stage {shown}/{last}[/]: {added}" + (f" [dim]({', '.join(new)})[/]" if new else ""))
    built = [f for f in features if f.stage <= shown]
    if built:
        console.print(f"  {len(built)}/{len(features)} features" +
                      (", game integration on" if any(f.game for f in built) else ""))
    result = _build(ctx, folder, ("Mirage",), verbose=verbose, warnings=warnings)
    if result.returncode == 0:
        _report_dll(folder)
        if shown < last:
            console.print(f"  [dim]Next: mirage minimal{' -c ' + config if config != 'debug' else ''} --next "
                          f"(stage {shown + 1}: {dict(all_stages).get(shown + 1, '?')})[/]")
    ctx.exit(result.returncode and 1)


@cli.command("verbose-build")
@config_option
@click.option("--from", "first", default="0", show_default=True, metavar="N", help="First stage to build and test.")
@click.option("--to", "last_stage", default="last", show_default=True, metavar="N|last", help="Last stage.")
@click.option("--seconds", default=2.0, show_default=True, help="How long the test host runs each phase.")
@click.option("-k", "--keep-going", is_flag=True, help="Test the remaining stages after a failure.")
@click.option("--no-unit-tests", is_flag=True, help="Skip the unit tests that run first.")
@click.option("--keep", is_flag=True, help="Keep every stage's DLL/PDB copies and test folders (dev mode: always).")
@verbose_option
@click.pass_context
def verbose_build(ctx: click.Context, config: str, first: str, last_stage: str, seconds: float, keep_going: bool,
                  no_unit_tests: bool, keep: bool, verbose: bool) -> None:
    """Build the menu in stages, adding features a few at a time, and test every stage thoroughly.

    \b
    First the unit tests, then for each stage of the staged build (mirage minimal --list):
      - build Mirage.dll and mirage_testhost.exe for that stage
      - check the build: identity, 32-bit, no Visual C++ runtime DLLs, compiler warnings
      - load it into the test host (a D3D9 app, no game needed) twice: hook, draw, open the menu, device reset,
        unload; then check the menu's log for every milestone, errors, crashes, failed debug checks, the log archive
        and the saved config
    It stops at the first failing stage (--keep-going: goes on) and writes build/verbose-build/<time>/report.md with
    everything needed to find the cause (written for Claude Code; a prompt pointing to it is copied to the
    clipboard). Outside the game, the game integration can't start: those errors are expected and allowed.
    """
    from . import devmode
    from . import stagetest as st

    all_stages = feat.stages()
    last = all_stages[-1][0]
    lo = int(first) if first.isdigit() else -1
    hi = last if last_stage.lower() == "last" else (int(last_stage) if last_stage.isdigit() else -1)
    if not (0 <= lo <= hi <= last):
        raise click.BadParameter(f"stages are 0-{last}, --from must not be after --to", ctx=ctx)
    keep = keep or devmode.enabled()
    # One run at a time: two runs reconfigure and rebuild the same staged build folder under each other
    lock = st.SESSIONS / f".running-{config}"
    st.SESSIONS.mkdir(parents=True, exist_ok=True)
    if lock.exists():
        other = lock.read_text(encoding="utf-8").strip()
        if other.isdigit() and str(int(other)) in subprocess.run(
                ["tasklist", "/FI", f"PID eq {other}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout:
            console.print(f"[bold red]Another verbose-build ({config}) is running[/] (pid {other}): it uses the same "
                          "build folder. Wait for it to finish (or stop it), then run this again.")
            ctx.exit(2)
    lock.write_text(str(os.getpid()), encoding="utf-8")
    ctx.call_on_close(lambda: lock.unlink(missing_ok=True))
    tc = _toolchain(ctx)
    session = st.SESSIONS / datetime.now().strftime("%Y%m%d-%H%M%S")
    session.mkdir(parents=True, exist_ok=True)
    argv = "mirage " + " ".join(sys.argv[1:])
    started = time.time()

    unit: tuple[bool, str] | None = None
    if not no_unit_tests:
        console.rule("Unit tests")
        _enable_parts(ctx, config, ["tests"], verbose)
        r = _build(ctx, config, ("mirage_tests", "installer_tests"), verbose=verbose)
        if r.returncode != 0:
            unit = (False, (r.log.read_text(encoding="utf-8", errors="replace") if r.log.exists() else "build failed"))
        else:
            p = subprocess.run([tc.ctest, "--test-dir", str(b.build_dir(config)), "--output-on-failure"], cwd=ROOT,
                               env=tc.env, capture_output=True, text=True, encoding="utf-8", errors="replace")
            unit = (p.returncode == 0, p.stdout + p.stderr)
        summary = next((line for line in unit[1].splitlines() if "tests passed" in line), "")
        console.print(f"  {'[green]passed[/]' if unit[0] else '[bold red]FAILED[/]'} [dim]{escape(summary)}[/]")

    results: list = []
    if unit is None or unit[0]:
        for stage, adds in all_stages:
            if not lo <= stage <= hi:
                continue
            new = [f.name for f in feat.load() if f.stage == stage]
            console.rule(f"Stage {stage}/{last}: {adds}")
            if new:
                console.print(f"  [dim]adds {', '.join(new)}[/]")
            r = st.test_stage(console, tc, config, stage, adds, session, seconds, verbose)
            results.append(r)
            st.print_stage(console, r)
            if not r.failed and not keep and r.folder and r.folder.exists():
                for big in list(r.folder.glob("*.dll")) + list(r.folder.glob("*.pdb")):
                    big.unlink(missing_ok=True)   # ~40 MB per stage in debug: only a failure's copies are kept
            if r.failed and not keep_going:
                break

    console.rule("Summary")
    if results:
        console.print(st.summary_table(results))
    failed = (unit is not None and not unit[0]) or any(r.failed for r in results)
    if failed:
        from .diaginfo import collect, copy_to_clipboard
        with console.status("Collecting debug info"):
            (session / "diag-info.txt").write_text(collect(), encoding="utf-8")
    report = st.write_report(session, config, results, unit, argv)
    console.print(f"Took {(time.time() - started) / 60:.1f} min. Report: [cyan]{report.relative_to(ROOT)}[/]")
    if failed:
        prompt = (f"`mirage verbose-build` failed. Read {report} (it has the failing stage, failed checks, build "
                  "diagnostics, the crash resolved to source lines, the menu's logs and the build setup), find the root "
                  "cause and fix it.")
        try:
            copy_to_clipboard(prompt)
            console.print("[bold red]Failed.[/] A prompt for Claude Code pointing to the report is on the clipboard.")
        except OSError:
            console.print(f"[bold red]Failed.[/] Give Claude Code this: {escape(prompt)}")
    else:
        console.print("[bold green]Every stage passed.[/]")
    old = sorted(p for p in st.SESSIONS.iterdir() if p.is_dir())
    for stale in old[:-10]:   # Keep the last 10 sessions
        shutil.rmtree(stale, ignore_errors=True)
    ctx.exit(1 if failed else 0)


def _stage_table(all_stages: list[tuple[int, str]], features: list[feat.Feature], current: int | None) -> Table:
    table = Table(title="Staged build (mirage minimal --stage N)", show_lines=True)
    table.add_column("Stage", justify="right", style="bold")
    table.add_column("Adds")
    table.add_column("Features", style="cyan")
    for n, what in all_stages:
        names = ", ".join(f.name for f in features if f.stage == n)
        mark = " [green]<- last build[/]" if n == current else ""
        table.add_row(str(n), what + mark, names)
    return table


@cli.command("features")
@config_option
def features_cmd(config: str) -> None:
    """List the features (cmake/Features.cmake) and which ones this build folder builds."""
    features = feat.load()
    state = feat.current_state(config, features)
    configured = b.cache_value(config, feat.MASTER) is not None
    table = Table(title=f"Features ({config}{'' if configured else ', not configured: defaults'})", box=None)
    table.add_column("Category", style="bold")
    table.add_column("Feature")
    table.add_column("Built")
    table.add_column("Needs game", style="dim")
    for category in feat.categories(features):
        for i, f in enumerate(x for x in features if x.category == category):
            table.add_row(category if i == 0 else "", f.name,
                          "[green]on[/]" if state[f.name] else "[red]off[/]", "yes" if f.game else "")
    console.print(table)
    console.print(f"{feat.summary(features, state)}\n[dim]Change with: mirage build -c {config} "
                  "--enable/--disable/--only NAME (a feature, a category or 'all')[/]")


@cli.command()
@config_option
@click.option("--warnings", "include_warnings", is_flag=True, help="List files with warnings too.")
@click.pass_context
def errors(ctx: click.Context, config: str, include_warnings: bool) -> None:
    """Build everything without stopping and list the errors grouped by file."""
    result = _build(ctx, config, keep_going=True, warnings="none")
    shown = result.diagnostics if include_warnings else result.errors
    if shown:
        console.print(b.error_table(shown, include_warnings))
    elif result.returncode == 0:
        console.print("[green]No errors.[/]")
    ctx.exit(result.returncode and 1)


@cli.command()
@config_option
@click.option("--no-build", is_flag=True, help="Run the tests as they were last built.")
@click.option("-R", "--filter", "name_filter", help="Only tests whose name matches this regex.")
@verbose_option
@click.pass_context
def test(ctx: click.Context, config: str, no_build: bool, name_filter: str | None, verbose: bool) -> None:
    """Build and run the unit tests (GoogleTest, through CTest)."""
    tc = _toolchain(ctx)
    if not no_build:
        _enable_parts(ctx, config, ["tests"], verbose)
        targets = ["mirage_tests", "installer_tests"]
        # unreallib_tests only exists with MIRAGE_UNREALLIB (its default follows MIRAGE_BUILD_TESTS on first configure)
        if b.option_enabled(config, "MIRAGE_UNREALLIB"):
            targets.append("unreallib_tests")
        if _build(ctx, config, tuple(targets), verbose=verbose).returncode != 0:
            ctx.exit(1)
    console.rule("Tests")
    code = b.run_ctest(console, tc, config, name_filter, verbose)
    ctx.exit(code and 1)


@cli.command("bench", context_settings={"ignore_unknown_options": True})
@click.option("-c", "--config", type=click.Choice(sorted(b.CONFIGS)), default="release", show_default=True,
              help="Build configuration (debug timings say little about the real thing).")
@click.option("--no-build", is_flag=True, help="Run the benchmarks as they were last built.")
@click.option("--list", "list_groups", is_flag=True, help="List the benchmark groups, then exit.")
@click.option("--csv", is_flag=True, help="Print the results as CSV instead of tables.")
@click.argument("groups", nargs=-1)
@verbose_option
@click.pass_context
def bench(ctx: click.Context, config: str, no_build: bool, list_groups: bool, csv: bool, groups: tuple[str, ...],
          verbose: bool) -> None:
    """Build and run the benchmarks (Google Benchmark): replays, macros, math, meshes.

    GROUPS limits the run to the groups whose name contains one of them (case-insensitive), e.g. `mirage bench replay`.
    Google Benchmark's own flags pass through, e.g. `mirage bench replay --benchmark_repetitions=5`.
    """
    if not no_build:
        _enable_parts(ctx, config, ["benchmarks"], verbose)
        if _build(ctx, config, ("mirage_bench",), verbose=verbose).returncode != 0:
            ctx.exit(1)
    exe = _artifact(config, "mirage_bench.exe")
    if not exe.exists():
        console.print(f"[red]{exe} not found[/]: build it first (without --no-build).")
        ctx.exit(1)
    if config == "debug":
        console.print("[yellow]Debug build: the timings are far slower than a release build's.[/]")
    args = [str(exe), *(["--list"] if list_groups else []), *(["--csv"] if csv else []), *groups]
    console.rule("Benchmarks")
    code = subprocess.run(args, cwd=ROOT).returncode
    ctx.exit(code and 1)


@cli.command("unreallib")
@config_option
@click.option("--no-build", is_flag=True, help="Run the check as it was last built.")
@click.argument("cooked_pc", required=False, type=click.Path(exists=True, file_okay=False, path_type=Path))
@verbose_option
@click.pass_context
def unreallib(ctx: click.Context, config: str, no_build: bool, cooked_pc: Path | None, verbose: bool) -> None:
    """Check that UnrealLib works: read, validate, resolve, decompile and load a map from the game's packages.

    COOKED_PC is the game's TdGame/CookedPC folder; found like the launcher finds the game when left out.
    """
    if not no_build:
        _enable_parts(ctx, config, ["tests"], verbose)
        if not b.option_enabled(config, "MIRAGE_UNREALLIB"):
            console.print("Enabling MIRAGE_UNREALLIB")
            result = b.configure(console, _toolchain(ctx), config, verbose=verbose, defines=("MIRAGE_UNREALLIB=ON",))
            if result.returncode != 0:
                console.print(f"[bold red]Configure failed[/] (log: {result.log})")
                ctx.exit(1)
        if _build(ctx, config, ("unreallib_status",), verbose=verbose).returncode != 0:
            ctx.exit(1)
    exe = _artifact(config, "unreallib_status.exe")
    if not exe.exists():
        console.print(f"[red]{exe} not found[/]: build it first (without --no-build).")
        ctx.exit(1)
    console.rule("UnrealLib")
    code = subprocess.run([str(exe), *([str(cooked_pc)] if cooked_pc else [])], cwd=ROOT).returncode
    ctx.exit(code and 1)


@cli.command("model")
@click.argument("model_file", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--json", "as_json", is_flag=True, help="The report as JSON.")
@click.option("--strict", is_flag=True, help="Exit with 1 when the model has problems for the game.")
@click.option("--convert", is_flag=True, help="Also write <file>.mmesh, the model in the game's space, which a mod's "
              "level layer places as a mesh (mesh = \"models/x.glb\").")
@click.option("--scale", type=float, default=50.0, show_default=True,
              help="With --convert: game units per file unit (glTF is in meters; a game unit is about 2 cm).")
@click.option("--no-build", is_flag=True, help="Run mirage_model as it was last built.")
@verbose_option
@click.pass_context
def model(ctx: click.Context, model_file: Path, as_json: bool, strict: bool, convert: bool, scale: float, no_build: bool,
          verbose: bool) -> None:
    """What a 3D model (glTF, FBX, OBJ) holds for the game: meshes, vertices, triangles, UVs, materials, size, and what
    a game static mesh can't take; with --convert, the model written for the game. Read with assimp by tools/model (its
    own build folder, always release: assimp never goes into Mirage.dll); the first build takes a few minutes."""
    tc = _toolchain(ctx)
    folder = b.ROOT / "cmake-build-x86-model"
    exe = folder / "mirage_model.exe"
    if not no_build:
        if not (folder / "build.ninja").exists():
            args = [tc.cmake, "-S", str(b.ROOT / "tools" / "model"), "-B", str(folder), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=cl.exe", "-DCMAKE_C_COMPILER=cl.exe"]
            if b.run_tool(console, tc, args, "Configuring [bold]tools/model[/]", folder / "mirage-configure.log",
                          verbose=verbose).returncode != 0:
                console.print(f"[red]Configuring tools/model failed[/]: {folder / 'mirage-configure.log'}")
                ctx.exit(2)
        if b.run_tool(console, tc, [tc.ninja, "-C", str(folder), "mirage_model"], "Building [bold]mirage_model[/]",
                      folder / "mirage-build.log", verbose=verbose).returncode != 0:
            console.print(f"[red]Building mirage_model failed[/]: {folder / 'mirage-build.log'}")
            ctx.exit(2)
    if not exe.exists():
        console.print(f"[red]{exe} not found[/]: build it first (without --no-build).")
        ctx.exit(2)
    args = [str(exe), str(model_file)] + (["--json"] if as_json else []) + (["--strict"] if strict else [])
    if convert:
        args += ["--convert", "--scale", str(scale)]
    ctx.exit(subprocess.run(args, cwd=b.ROOT).returncode)


@cli.command("sdk")
@config_option
@click.option("--no-build", is_flag=True, help="Run mirage_sdkgen as it was last built.")
@click.option("--no-check", is_flag=True, help="Don't compile the generated SDK afterwards (its layout checks).")
@click.option("--no-report", is_flag=True, help="Don't compare with the old (mmultiplayer) SDK.")
@click.option("--metadata", type=click.Path(dir_okay=False, path_type=Path),
              help="Export merged SDK metadata JSON without regenerating headers.")
@click.argument("sdkgen_args", nargs=-1, type=click.UNPROCESSED)
@verbose_option
@click.pass_context
def sdk(ctx: click.Context, config: str, no_build: bool, no_check: bool, no_report: bool,
        metadata: Path | None, sdkgen_args: tuple[str, ...], verbose: bool) -> None:
    """Generate the Mirage SDK (sdk/) from the game's packages with mirage_sdkgen, then compile it.

    Settings are in tools/sdkgen/sdkgen.toml; SDKGEN_ARGS go to mirage_sdkgen (after --, e.g. `mirage sdk --
    --cooked D:/Games/MirrorsEdge/TdGame/CookedPC`). The report of what the old SDK has wrong goes to
    build/sdk-reference/old-sdk-report.md, with the decompiled script and the Lua stubs.
    """
    if not no_build:
        _enable_parts(ctx, config, ["sdkgen"], verbose)
        if _build(ctx, config, ("mirage_sdkgen",), verbose=verbose).returncode != 0:
            ctx.exit(1)
    exe = _artifact(config, "bin/mirage_sdkgen.exe")
    if not exe.exists():
        console.print(f"[red]{exe} not found[/]: build it first (without --no-build).")
        ctx.exit(1)
    args = [str(exe)]
    if metadata is not None:
        metadata = metadata.resolve()
        metadata.parent.mkdir(parents=True, exist_ok=True)
        args += ["--metadata-only", str(metadata)]
    old = b.cache_value(config, "CPM_PACKAGE_mmultiplayer_SOURCE_DIR")
    if not no_report and old and (Path(old) / "Client" / "sdk").is_dir():
        args += ["--old-sdk", str(Path(old) / "Client" / "sdk")]
    console.rule("mirage_sdkgen")
    if subprocess.run([*args, *sdkgen_args], cwd=ROOT).returncode != 0:
        ctx.exit(1)
    if no_check or metadata is not None or "--metadata-only" in sdkgen_args:
        return
    # The SDK's CMakeLists is picked up by tools/sdkgen when it exists: configure again the first time
    console.rule("Compiling the SDK (every offset and size is a static_assert)")
    result = b.configure(console, _toolchain(ctx), config, verbose=verbose)
    if result.returncode != 0 or _build(ctx, config, ("mirage_sdk",), verbose=verbose).returncode != 0:
        ctx.exit(1)


@cli.command("sdk-browse")
@click.argument("query", required=False)
@click.option("-m", "--members", is_flag=True, help="Find fields and functions by name in every type instead.")
@click.option("-i", "--inherited", is_flag=True, help="Also the fields of what the type extends, by offset.")
@click.option("-f", "--functions", is_flag=True, help="List the type's functions too.")
@click.option("-g", "--gui", is_flag=True, help="Open the SDK browser window (PyQt6), at QUERY if given.")
@click.option("--api-output", type=click.Path(file_okay=False, path_type=Path),
              help="Read API generation metadata from this directory in the SDK browser.")
@click.pass_context
def sdk_browse(ctx: click.Context, query: str | None, members: bool, inherited: bool, functions: bool, gui: bool,
               api_output: Path | None) -> None:
    """Browse the generated SDK (sdk/include/sdk): a type's layout, or the types and members matching QUERY.

    QUERY is a C++ name (ATdPawn), an UnrealScript name (TdPawn) or a path (TdGame.TdPawn); a single match is shown
    whole (offsets, sizes, types, flags, what it extends), several are listed. Type::Member finds a member on its
    declaring type. --gui opens a window instead.
    """
    if gui:
        from . import sdkbrowse_gui
        ctx.exit(sdkbrowse_gui.run(query, api_output))
    if not query:
        raise click.UsageError("QUERY is needed without --gui")
    from . import sdkbrowse as sb

    types = sb.load()
    if not types:
        console.print(f"[red]No SDK headers in {sb.SDK_DIR}[/] (run `mirage sdk`)")
        raise SystemExit(1)

    if members or sb.QUALIFIED_MEMBER.fullmatch(query):
        found = sb.find_members(types, query)
        table = Table("Type", "Member", "Offset", "Type / signature", box=None)
        for column in table.columns[:3]:
            column.no_wrap = True
        for t, m in found[:300]:
            if isinstance(m, sb.Field):
                table.add_row(t.cpp, m.name, f"0x{m.offset:04X}", escape(m.type))
            else:
                table.add_row(t.cpp, sb.function_name(m), "", escape(m))
        console.print(table)
        console.print(f"[dim]{len(found)} member(s){' (the first 300)' if len(found) > 300 else ''}[/]")
        return

    found = sb.find(types, query)
    if not found:
        console.print(f"[yellow]No type matches '{escape(query)}'[/] (try -m to search members)")
        raise SystemExit(1)
    if len(found) > 1:
        table = Table("Kind", "C++", "Path", "Size", "Where", box=None)
        for t in found[:300]:
            table.add_row(t.kind, t.cpp, t.path, f"0x{t.size:04X}" if t.size is not None else "", f"{t.file}:{t.line}")
        console.print(table)
        console.print(f"[dim]{len(found)} type(s){' (the first 300)' if len(found) > 300 else ''}; name one to see it[/]")
        return

    t = found[0]
    size = f", 0x{t.size:04X} bytes" if t.size is not None else ""
    console.print(f"[bold]{t.kind} {t.path}[/] ({t.cpp}{size})  [dim]sdk/include/sdk/{t.file}:{t.line}[/]")
    if t.kind == "Enum":
        for name, value in t.values:
            console.print(f"  {value:>4}  {name}")
        return
    bases = sb.chain(types, t)
    if len(bases) > 1:
        console.print("[dim]" + " > ".join(b_.cpp for b_ in bases) + "[/]")
    table = Table("Offset", "Size", "Type", "Name", "Flags", *(["From"] if inherited else []), box=None)
    table.columns[2].no_wrap = True
    rows = [(f, b_) for b_ in (bases if inherited else [t]) for f in b_.fields]
    for f, b_ in sorted(rows, key=lambda r: r[0].offset):
        table.add_row(f"0x{f.offset:04X}", f"0x{f.size:X}", escape(f.type), f.name, f"[dim]{escape(f.annotations)}[/]",
                      *([b_.cpp] if inherited else []))
    console.print(table)
    if functions:
        for fn in t.functions:
            console.print("  " + escape(fn))
    console.print(f"[dim]{len(rows)} field(s), {len(t.functions)} function(s)"
                  f"{'' if functions else ' (-f lists them)'}{'' if inherited else '; -i adds inherited fields'}[/]")


INDEX_DB = ROOT / "build" / "content-index.sqlite"


@cli.command("index")
@config_option
@click.argument("pattern", required=False)
@click.option("--class", "cls", help="Only objects of this class (e.g. Material, PortalTeleporter).")
@click.option("--sql", help="Run this SQL on the table objects(package, path, class, outer, size) instead.")
@click.option("--limit", default=50, show_default=True, help="Rows to show.")
@click.option("--rebuild", is_flag=True, help="Index the game's packages again.")
@verbose_option
@click.pass_context
def index(ctx: click.Context, config: str, pattern: str | None, cls: str | None, sql: str | None, limit: int,
          rebuild: bool, verbose: bool) -> None:
    """Search every object in the game's cooked packages (built once into build/content-index.sqlite by mirage_index).

    `mirage index portal` finds paths containing "portal"; `mirage index --class TextureSampleParameter2D`;
    `mirage index --sql "select outer from objects where class = 'MaterialExpressionScreenPosition'"`.
    """
    import sqlite3

    if rebuild or not INDEX_DB.exists():
        game = _find_game_dir()
        cooked = game.parent / "TdGame" / "CookedPC" if game else None
        if not cooked or not cooked.is_dir():
            console.print("[red]The game's TdGame/CookedPC wasn't found[/].")
            ctx.exit(1)
        _enable_parts(ctx, config, ["sdkgen"], verbose)
        if _build(ctx, config, ("mirage_index",), verbose=verbose).returncode != 0:
            ctx.exit(1)
        tsv = b.build_dir(config) / "content-index.tsv"
        console.rule(f"Indexing {cooked}")
        if subprocess.run([str(_artifact(config, "bin/mirage_index.exe")), str(cooked), str(tsv)]).returncode != 0:
            ctx.exit(1)
        INDEX_DB.parent.mkdir(exist_ok=True)
        INDEX_DB.unlink(missing_ok=True)
        with sqlite3.connect(INDEX_DB) as db, tsv.open(encoding="utf-8", errors="replace") as rows:
            db.execute("create table objects(package text, path text, class text, outer text, size integer)")
            db.executemany("insert into objects values (?, ?, ?, ?, ?)",
                           (line.rstrip("\n").split("\t") for line in rows if line.count("\t") == 4))
            db.execute("create index objects_class on objects(class)")
            db.execute("create index objects_outer on objects(outer)")
        console.print(f"Index: {INDEX_DB.relative_to(ROOT)}")

    query, args = sql, ()
    if not query:
        where = [w for w, on in (("path like ?", pattern), ("class = ?", cls)) if on]
        args = tuple(v for v in (f"%{pattern}%" if pattern else None, cls) if v)
        query = "select package, path, class, size from objects" + (" where " + " and ".join(where) if where else "")
        if not where and not rebuild:
            query = "select class, count(*) n from objects group by class order by n desc"
    with sqlite3.connect(INDEX_DB) as db:
        cursor = db.execute(query, args)
        table = Table(*[d[0] for d in cursor.description])
        rows = cursor.fetchmany(limit)
        for row in rows:
            table.add_row(*(str(v) for v in row))
    console.print(table)
    if len(rows) == limit:
        console.print(f"(first {limit} rows: --limit for more)")


ASSET_DB = ROOT / "build" / "asset-catalog.sqlite"


@cli.command("assets")
@config_option
@click.argument("query", required=False)
@click.option("--rebuild", is_flag=True, help="Read the game's assets again (after a game update).")
@verbose_option
@click.pass_context
def assets(ctx: click.Context, config: str, query: str | None, rebuild: bool, verbose: bool) -> None:
    """Browse the game's textures, sounds, meshes, animations and materials in a window, with their details.

    The catalog is built once by mirage_assets (a few minutes) into build/asset-catalog.sqlite; previews (a texture's
    image, a sound) are made when an asset is picked, into build/asset-previews. QUERY fills the search. Export CSV
    saves all matches for the current search and filters.
    """
    game = _find_game_dir()
    cooked = game.parent / "TdGame" / "CookedPC" if game else None
    if not cooked or not cooked.is_dir():
        console.print("[red]The game's TdGame/CookedPC wasn't found[/].")
        ctx.exit(1)
    tool = _artifact(config, "bin/mirage_assets.exe")
    if rebuild or not ASSET_DB.exists() or not tool.exists():
        _enable_parts(ctx, config, ["sdkgen"], verbose)
        if _build(ctx, config, ("mirage_assets",), verbose=verbose).returncode != 0:
            ctx.exit(1)
    if rebuild or not ASSET_DB.exists():
        from .assets_gui import load_catalog
        jsonl = b.build_dir(config) / "asset-catalog.jsonl"
        console.rule(f"Reading the assets of {cooked}")
        if subprocess.run([str(tool), "catalog", str(cooked), str(jsonl)]).returncode != 0:
            ctx.exit(1)
        ASSET_DB.parent.mkdir(exist_ok=True)
        try:
            console.print(f"{load_catalog(jsonl, ASSET_DB):,} assets in {ASSET_DB.relative_to(ROOT)}")
        except PermissionError:
            console.print(f"[red]{ASSET_DB.relative_to(ROOT)} is open in an asset window[/]: close it and run "
                          "`mirage assets --rebuild` again.")
            ctx.exit(1)
        shutil.rmtree(ROOT / "build" / "asset-previews", ignore_errors=True)   # Made from the old catalog's packages
    from . import assets_gui
    ctx.exit(assets_gui.run(ASSET_DB, tool, cooked, ROOT / "build" / "asset-previews", query))


def _remove_build_dir(config: str) -> None:
    folder = b.build_dir(config)
    if folder.exists():
        with console.status(f"Removing {folder.relative_to(ROOT)}"):
            shutil.rmtree(folder)
        console.print(f"Removed {folder.relative_to(ROOT)}")


@cli.command()
@config_option
@click.option("--all", "all_configs", is_flag=True, help="Every configuration, and the cached MSVC environment.")
@click.option("-y", "--yes", is_flag=True, help="Don't ask.")
def clean(config: str, all_configs: bool, yes: bool) -> None:
    """Delete build folders (the dependency download cache is kept)."""
    configs = sorted(b.PRESETS) if all_configs else [config]
    folders = [b.build_dir(c) for c in configs if b.build_dir(c).exists()]
    if not folders and not all_configs:
        console.print("Nothing to clean.")
        return
    names = ", ".join(str(f.relative_to(ROOT)) for f in folders) or "the MSVC environment cache"
    if not yes and not click.confirm(f"Delete {names}?", default=True):
        return
    for c in configs:
        _remove_build_dir(c)
    if all_configs:
        (ROOT / ".cache" / "msvc-x86-env.json").unlink(missing_ok=True)


@cli.command("install-game")
@config_option
@click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="Game folder (default: detected).")
@click.option("--no-build", is_flag=True, help="Install what was built last.")
@click.option("--gui", is_flag=True, help="Open the installer window instead of installing right away.")
@click.pass_context
def install_game(ctx: click.Context, config: str, game_dir: Path | None, no_build: bool, gui: bool) -> None:
    """Build, then install the menu into Mirror's Edge (asks for admin rights)."""
    _run_installer(ctx, config, ["--install"], game_dir, no_build, gui)


@cli.command("uninstall-game")
@config_option
@click.option("--dir", "game_dir", type=click.Path(path_type=Path), help="Game folder (default: detected).")
@click.option("--remove-settings", is_flag=True, help="Also delete the menu's config, themes and logs.")
@click.pass_context
def uninstall_game(ctx: click.Context, config: str, game_dir: Path | None, remove_settings: bool) -> None:
    """Remove the menu from Mirror's Edge (asks for admin rights)."""
    args = ["--uninstall"] + (["--remove-settings"] if remove_settings else [])
    _run_installer(ctx, config, args, game_dir, no_build=True, gui=False)


def _run_installer(ctx: click.Context, config: str, args: list[str], game_dir: Path | None, no_build: bool,
                   gui: bool) -> None:
    if not no_build:
        _enable_parts(ctx, config, ["installer"])
        if _build(ctx, config, ("mirage_installer",)).returncode != 0:
            ctx.exit(1)
    installer = _artifact(config, INSTALLER)
    if not installer.exists():
        console.print(f"[red]{installer.relative_to(ROOT)} has not been built yet.[/] Run: mirage build -c {config}")
        ctx.exit(1)
    if gui:
        args = []
    else:
        args = args + ["--quiet"] + ([str(game_dir.resolve())] if game_dir else [])
    console.print("Waiting for the installer (accept the administrator prompt)...")
    code = run_elevated(installer, args)
    if code == 0:
        console.print("[bold green]Done.[/]" if gui else
                      f"[bold green]Done:[/] {'installed into' if '--install' in args else 'removed from'} the game. "
                      "Start Mirror's Edge normally; INSERT opens the menu.")
    elif code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
    else:
        console.print("[bold red]The installer failed[/] (is the game running?). Run with --gui to see the reason.")
    ctx.exit(0 if code == 0 else 1)


@cli.command(context_settings={"ignore_unknown_options": True, "allow_extra_args": True})
@config_option
@click.option("--no-build", is_flag=True, help="Use what was built last.")
@click.pass_context
def launch(ctx: click.Context, config: str, no_build: bool) -> None:
    """Inject without installing: run the launcher (extra arguments are passed on, e.g. --no-start)."""
    if not no_build:
        _enable_parts(ctx, config, ["launcher"])
        if _build(ctx, config, ("mirage_launcher",)).returncode != 0:
            ctx.exit(1)
    launcher = _artifact(config, LAUNCHER)
    if not launcher.exists():
        console.print(f"[red]{launcher.relative_to(ROOT)} has not been built yet.[/] Run without --no-build.")
        ctx.exit(1)
    code = _run_launcher(launcher, list(ctx.args))
    if code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
        ctx.exit(1)
    console.print("[bold green]Launcher finished.[/]" if code == 0 else f"[bold red]The launcher failed[/] (exit {code})")
    ctx.exit(0 if code == 0 else 1)


def _run_launcher(launcher: Path, args: list[str]) -> int:
    """Runs the launcher (it always runs as administrator). Returns its exit code, -1 when the prompt was declined."""
    if is_elevated():
        return subprocess.run([str(launcher), *args], cwd=launcher.parent).returncode
    console.print("Starting the launcher as administrator (accept the prompt; its output is in its own window, "
                  "press Enter there when it is done)...")
    return run_elevated(launcher, args)


@cli.command("dev-test", context_settings={"ignore_unknown_options": True, "allow_extra_args": True})
@config_option
@click.option("-s", "--stage", default=None, metavar="N|last",
              help="Test the staged build at this stage (mirage minimal) instead of the normal build.")
@click.option("--no-build", is_flag=True, help="Test what was built last.")
@click.option("--no-wait", is_flag=True, help="Don't wait for the game to close (no report).")
@click.pass_context
def dev_test(ctx: click.Context, config: str, stage: str | None, no_build: bool, no_wait: bool) -> None:
    """Build, start Mirror's Edge with the freshly built menu injected, and report how the run went once you quit.

    \b
    1. builds Mirage.dll (or the staged build, --stage) and the launcher
    2. runs the launcher (as administrator): it starts the game if it isn't running, and injects the DLL
    3. waits until the game is closed, then shows the menu's log and, if the game crashed, where: Windows' crash
       record, the function and source line through the PDB, and the crash dump
    Extra arguments go to the launcher (e.g. --steam, --no-start). The build folder keeps its features and options:
    change them with `mirage build` (or `mirage minimal`) first.
    """
    from .devtest import crashes_since, dumps_since, game_pids, log_since, symbolize, wait_for_game_exit

    folder = config
    if stage is not None:
        folder = b.MINIMAL[config]
        last = feat.stages()[-1][0]
        stage = str(last) if stage.lower() == "last" else stage
        if not stage.isdigit() or int(stage) > last:
            raise click.BadParameter(f"must be 0-{last} or 'last'", ctx=ctx, param_hint="--stage")

    launcher = _artifact(config, LAUNCHER)
    if not no_build:
        _enable_parts(ctx, config, ["launcher"])
        if _build(ctx, config, ("mirage_launcher",)).returncode != 0:
            ctx.exit(1)
        if stage is not None:
            if b.cache_value(folder, "MIRAGE_MINIMAL_STAGE") != stage:
                result = b.configure(console, _toolchain(ctx), folder, defines=(f"MIRAGE_MINIMAL_STAGE={stage}",))
                if result.returncode != 0:
                    console.print(f"[bold red]Configure failed[/] (log: {result.log})")
                    ctx.exit(1)
            console.print(f"Staged build, stage {stage}: {dict(feat.stages()).get(int(stage), '?')}")
            if _build(ctx, folder, ("Mirage",)).returncode != 0:
                ctx.exit(1)

    # The DLL straight from its build folder: its PDB is next to it, and so is the logs/mirage.log it writes.
    dll = _artifact(folder, "Mirage.dll")
    for path, what in ((launcher, "launcher"), (dll, "DLL")):
        if not path.exists():
            console.print(f"[red]No {what} at {path}.[/] Run without --no-build.")
            ctx.exit(1)
    console.print(f"[bold]Injecting:[/] [cyan]{dll}[/]")
    if game_pids():
        console.print("[yellow]Mirror's Edge is already running:[/] the menu goes into that game. If a Mirage.dll "
                      "is already loaded there, the launcher does nothing (close the game for a clean test).")

    start = datetime.now()
    code = _run_launcher(launcher, ["--dll", str(dll), *ctx.args])
    if code == -1:
        console.print("[yellow]The administrator prompt was declined.[/]")
        ctx.exit(1)
    if code != 0:
        console.print(f"[bold red]The launcher failed[/] (exit {code}): see its window for the reason.")
    if no_wait:
        ctx.exit(0 if code == 0 else 1)

    if game_pids():
        console.print("Game running. [bold]Quit Mirror's Edge to see the report[/] (Ctrl+C: stop waiting).")
        try:
            with console.status("Waiting for Mirror's Edge to close"):
                wait_for_game_exit()
        except KeyboardInterrupt:
            console.print("Stopped waiting: no report.")
            ctx.exit(0)
        time.sleep(3)   # Windows writes the crash record a moment after the process is gone

    # ---- report -----------------------------------------------------------------------------------------------------
    console.rule("Dev test report")
    lines = log_since(dll, start)
    if lines is None:
        console.print(f"[bold red]The menu never started:[/] no logs/mirage.log was written next to the DLL "
                      f"({dll.parent}). If the launcher said 'already loaded', another Mirage.dll was in the game "
                      "(the dinput8 loader's installed copy, or an earlier injection).")
    else:
        console.print(f"[bold]mirage.log[/] (last {min(len(lines), 20)} of {len(lines)} lines):")
        for line in lines[-20:]:
            style = "red" if "[error]" in line or "[critical]" in line else "yellow" if "[warning]" in line else "dim"
            console.print(f"  [{style}]{escape(line)}[/]")
        if not any("Unloading" in line for line in lines):
            console.print("  [yellow](the log ends without 'Unloading': the menu didn't unload, the game was closed "
                          "or crashed with it loaded)[/]")

    crashes = crashes_since(start)
    if not crashes:
        console.print("[bold green]No crash recorded.[/]")
    for c in crashes:
        console.print(f"[bold red]Crash[/] at {c.time}: [bold]{c.module}+{c.offset:#x}[/], exception {c.code}"
                      + (f" ({c.description})" if c.description else ""))
        if c.module.lower() == "Mirage.dll":
            where = symbolize(Path(c.module_path) if c.module_path else dll, c.offset)
            console.print(f"  at {escape(where)}" if where else "  [dim](no symbols for that build)[/]")
            if where and "\\rtc\\" in where.lower():
                console.print("  [dim]a /RTC run-time check fired there: the code that tripped it is further up the "
                              "stack, in the crash dump[/]")
    for dump in dumps_since(start):
        console.print(f"  dump: [cyan]{dump}[/]")
    ctx.exit(1 if crashes or code != 0 else 0)


@cli.command("game-test")
@config_option
@click.option("--no-build", is_flag=True, help="Test what was built last.")
@click.option("-B", "--build-dir", type=click.Path(path_type=Path, file_okay=False),
              help="Take Mirage.dll and loader/dinput8.dll from this build folder (implies --no-build).")
@click.option("--dir", "game_dir", type=click.Path(path_type=Path, file_okay=False), help="Game folder (default: detected).")
@click.option("--map", "map_name", default="edge_p", show_default=True,
              help="Level to load before testing the modules ('none': stay in the main menu).")
@click.option("--checkpoint", default=2, show_default=True,
              help="Checkpoint of that level to start at (0: the level start; the Prologue's first two are its intro).")
@click.option("--ticks", default=10, show_default=True, help="Engine ticks (and frames) each module stays on.")
@click.option("--only", default="", metavar="A,B", help="Test only these modules (by name).")
@click.option("--skip", default="", metavar="A,B", help="Don't test these modules.")
@click.option("--late", "late_s", default=0.0, metavar="SECONDS",
              help="Load the menu this much later than usual (after the game went fullscreen, like the launcher).")
@click.option("--timeout", default=900.0, show_default=True, help="Kill the game after this many seconds.")
@click.option("--keep-open", is_flag=True, help="Don't close the game when the test is done.")
@click.option("--minimized/--visible", default=False, show_default=True,
              help="Start the game minimized, without taking focus (default: visible, windowed; Left Alt + Right Alt "
                   "send it to the taskbar).")
@click.option("--real-speed", is_flag=True,
              help="Test the game as players run it: the 62 fps cap stays on and TAS playbacks are paced (slower).")
@click.option("--retries", default=1, show_default=True,
              help="Run again when the game itself didn't get going (Steam error, no frame drawn): not for failures.")
@click.option("--safe-mode", is_flag=True,
              help="Start in safe mode (the start marker planted, as after a crashed start): no mod loads; run with "
                   "--only \"Mirage safe mode\".")
@click.option("--mod-tests", default="", metavar="ID,ID",
              help="The step \"Mod tests\" runs only these mods' own tests (default: every installed mod's).")
@click.pass_context
def game_test(ctx: click.Context, config: str, no_build: bool, build_dir: Path | None, game_dir: Path | None,
              map_name: str, checkpoint: int, ticks: int, only: str, skip: str, late_s: float, timeout: float,
              keep_open: bool, minimized: bool, real_speed: bool, retries: int, mod_tests: str = "",
              safe_mode: bool = False) -> None:
    """Run the in-game self-test in Mirror's Edge and report every step (see tools/mirage_tools/gametest.py).

    \b
    Installs the build into the game folder (loader + menu, like the installer, no admin needed when the folder is
    writable), starts the game, and the menu tests itself: hooks and drawing, the engine tick, loading a level, then
    every module switched on and off (all its key-bindable controls pressed, menu open and closed), all modules on at
    once, and a clean unload. Then it closes the game. Exit code 0 only when every step passed and nothing crashed.
    """
    from . import devtest
    from . import gametest as gt

    if layers := gt.compat_layers():
        console.print(f"[yellow]MirrorsEdge.exe has compatibility settings ({escape(layers)})[/]: \"Run as "
                      "administrator\" and the Windows XP/98 modes make the game start elevated, which this tool "
                      "can't start or watch. Clear them in the exe's Properties > Compatibility, or run this from an "
                      "administrator terminal.")
    if gt.LINUX:
        options = gt.linux_launch_options() or ""
        if "dinput8=n,b" not in options.replace(" ", ""):
            console.print("[red]The game's Steam launch options don't load the menu's dinput8.dll[/] (Proton uses its "
                          "own unless told). In Steam: Mirror's Edge > Properties > General > Launch Options:\n"
                          "    WINEDLLOVERRIDES=\"dinput8=n,b\" %command%\n"
                          f"Now: {escape(options) if options else 'none'}. Steam saves the options to disk when it "
                          "exits, so restart Steam once after setting them. See docs/linux-development.md.")
            ctx.exit(1)
    game_dir = game_dir or gt.find_game_dir()
    if not game_dir or not (game_dir / devtest.GAME_EXE).exists():
        console.print("[red]Mirror's Edge not found.[/] Pass --dir <...\\Binaries>" +
                      (" or <...>/steamapps/common/mirrors edge/Binaries." if gt.LINUX else "."))
        ctx.exit(1)

    if build_dir:
        folder = build_dir if build_dir.is_absolute() else ROOT / build_dir
    else:
        folder = b.build_dir(config)
        if not no_build:
            _enable_parts(ctx, config, ["installer"])   # The loader comes with the installer option
            if _build(ctx, config, ("Mirage", "mirage_loader")).returncode != 0:
                ctx.exit(1)
    dll, loader = folder / "Mirage.dll", folder / "loader" / "dinput8.dll"
    for f in (dll, loader):
        if not f.exists():
            console.print(f"[red]{f} is missing[/]: build with the installer part (mirage build --with installer).")
            ctx.exit(1)
    if devtest.game_pids():
        console.print("[red]Mirror's Edge is running.[/] Close it first.")
        ctx.exit(1)
    # The mods the self-test loads (steps "Mirage mods" and "Mirage lifecycle"): the native ones built like a mod
    # author would (their own CMake project, clang-cl when installed), the Lua ones as they are
    test_mods, how = em.for_self_test(_toolchain(ctx))
    if test_mods is None:
        console.print(f"[red]A test mod didn't build:[/]\n{escape(how)}")
        ctx.exit(1)
    console.print(f"Test mods: {how}")
    if (game_dir / "mirage" / "disabled").exists():
        console.print("[yellow]mirage\\disabled exists: the loader would skip the menu. Removed it.[/]")
        (game_dir / "mirage" / "disabled").unlink()

    console.print(f"Testing [cyan]{dll}[/] in [cyan]{game_dir}[/]" + (f", loaded {late_s:.0f} s late" if late_s else "")
                  + (", at real speed" if real_speed else ""))

    def progress(res: gt.Result) -> None:
        if res.current:
            console.print(f"  [dim]{res.current}...[/]")

    attempt = 0
    while True:
        gt.install(game_dir, dll, loader)
        em.install_for_test(game_dir, test_mods)
        # Safe mode (step 10): <mods>/starting.toml left from a start that didn't finish makes the next start load no mods.
        # A stale one (a run killed before its level loaded) is cleared; --safe-mode plants one on purpose.
        marker = game_dir / "mirage" / "mods" / "starting.toml"
        if safe_mode:
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text('reason = "planted by mirage game-test --safe-mode"\n', encoding="utf-8")
        elif marker.exists():
            console.print("[yellow]A start marker was left by a run that didn't finish its start: removed it (no safe mode).[/]")
            marker.unlink()
        gt.write_options(game_dir, {"map": map_name, "checkpoint": str(checkpoint), "ticks": str(ticks), "only": only,
                                    "skip": skip, "fast": "0" if real_speed else "1", "modtests": mod_tests,
                                    "safemode": "1" if safe_mode else "",
                                    # The menu never closes the game itself: gt.run does, once the menu has unloaded
                                    "quit": "0"}, int(late_s * 1000))
        try:
            res = gt.run(game_dir, timeout, progress, close=not keep_open, minimized=minimized)
        except gt.ElevationRequired:
            console.print("[red]Windows wants administrator rights to start the game[/] (compatibility settings on "
                          "MirrorsEdge.exe, see above): run this from an administrator terminal, or clear them.")
            ctx.exit(1)
        finally:
            gt.cleanup(game_dir)
            em.remove_test_mods(game_dir)
        if res.ok or not res.environment or attempt >= retries:
            break
        attempt += 1
        console.print(f"[yellow]Not the menu: {escape(res.environment)}.[/] Trying again ({attempt} of {retries})...")
        time.sleep(3.0)

    table = Table(show_header=True, header_style="bold")
    table.add_column("Step")
    table.add_column("Result")
    table.add_column("Time", justify="right")
    table.add_column("Details")
    for s in res.steps:
        details = "\n".join(s.get("problems", []) + [f"[dim]{n}[/]" for n in s.get("notes", [])][:12])
        table.add_row(escape(s["name"]), "[green]ok[/]" if s["ok"] else "[bold red]FAILED[/]", f"{s['seconds']:.1f}s",
                      details)
    console.print(table)
    if res.current:
        console.print(f"[bold red]Stopped in the middle of: {escape(res.current)}[/]")
    console.print(f"{res.exit_reason}, {res.seconds:.0f} s")
    for popup in res.popups:
        console.print(f"[bold red]Error popup[/] (closed): {escape(popup)}")
    if res.hang and res.hang.stacks:
        console.print("[yellow]The menu's last thread stacks before the hang:[/]")
        for line in res.hang.stacks[-12:]:
            console.print(f"  {escape(line[:300])}")
    for c in res.crashes:
        where = devtest.symbolize(game_dir / "mirage" / "Mirage.dll", c.offset) if "Mirage" in c.module.lower() else None
        console.print(f"[bold red]Crash[/] {c.time} {c.code} {c.description} in {c.module}+{c.offset:#x}"
                      + (f"\n  at {escape(where)}" if where else ""))
    if res.log_errors:
        console.print(f"[yellow]{len(res.log_errors)} error line(s) in the menu's log:[/]")
        for line in res.log_errors[:20]:
            console.print(f"  {escape(line)}")
    ok = res.ok
    if not ok and res.environment:
        console.print(f"[yellow]The game itself didn't get going: {escape(res.environment)}[/] (not a menu failure; "
                      "for a Steam error, restart Steam)")
    slow = sorted(res.steps, key=lambda s: -s.get("seconds", 0.0))[:3]
    if slow and ok:
        console.print("[dim]Slowest: " + ", ".join(f"{escape(s['name'])} {s['seconds']:.1f}s" for s in slow) + "[/]")
    console.print("[bold green]Self-test passed.[/]" if ok else "[bold red]Self-test failed.[/]")
    ctx.exit(0 if ok else 1)


@cli.command()
@click.option("--no-build", is_flag=True, help="Package what was built last.")
@click.option("--dll", "dll_only", is_flag=True,
              help="Only the menu's DLL: build the release Mirage.dll and copy it to dist/ (no installer or zips).")
@click.pass_context
def package(ctx: click.Context, no_build: bool, dll_only: bool) -> None:
    """Build the release installer and copy it to dist/, with the zip to hand out (installer, launcher, readme, licenses)."""
    if dll_only:
        _export_dll(ctx, no_build)
        return
    if not no_build:
        _enable_parts(ctx, "release", ["installer", "launcher"])
        if _build(ctx, "release", ("mirage_installer", "mirage_launcher")).returncode != 0:
            ctx.exit(1)
    src = _artifact("release", INSTALLER)
    if not src.exists():
        console.print("[red]No release installer yet.[/] Run without --no-build.")
        ctx.exit(1)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    # Versioned name for people browsing releases, fixed name for install.ps1 (releases/latest/download/<name>).
    versioned = dist / f"mirage_installer-{project_version()}.exe"
    fixed = dist / INSTALLER
    shutil.copy2(src, versioned)
    shutil.copy2(src, fixed)
    digest = hashlib.sha256(versioned.read_bytes()).hexdigest()

    # The same two files unpacked, laid out like the game's Binaries folder, for install.sh (Linux / Steam Deck /
    # macOS: the game runs in Proton or Wine there, where the installer exe can't be used).
    files = dist / FILES_ZIP
    loader = b.build_dir("release") / "loader" / "dinput8.dll"
    menu = _artifact("release", "Mirage.dll")
    for path in (loader, menu):
        if not path.exists():
            console.print(f"[red]{path.relative_to(ROOT)} is missing.[/] Run without --no-build.")
            ctx.exit(1)
    with zipfile.ZipFile(files, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(loader, "dinput8.dll")
        z.write(menu, "mirage/Mirage.dll")
    files_digest = hashlib.sha256(files.read_bytes()).hexdigest()

    # The zip to hand out: everything in one folder, the installer first.
    launcher = _artifact("release", LAUNCHER)
    if not launcher.exists():
        console.print(f"[red]{launcher.relative_to(ROOT)} is missing.[/] Run without --no-build.")
        ctx.exit(1)
    try:
        notices = _third_party_notices("release")
    except FileNotFoundError as e:
        console.print(f"[red]Can't write the license notices:[/] {e}")
        ctx.exit(1)
    bundle = dist / f"MirrorsEdgeMenu-{project_version()}.zip"
    top = f"MirrorsEdgeMenu-{project_version()}"
    readme = (DIST_FILES / "README.txt").read_text(encoding="utf-8").replace("{version}", project_version())
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(src, f"{top}/{INSTALLER}")
        z.writestr(f"{top}/README.txt", _crlf(readme))
        z.writestr(f"{top}/LICENSE.txt", _crlf((ROOT / "LICENSE").read_text(encoding="utf-8")))
        z.writestr(f"{top}/THIRD-PARTY-NOTICES.txt", _crlf(notices))
        z.write(launcher, f"{top}/try-without-installing/{LAUNCHER}")
        z.write(menu, f"{top}/try-without-installing/Mirage.dll")
    bundle_digest = hashlib.sha256(bundle.read_bytes()).hexdigest()

    (dist / "SHA256SUMS.txt").write_text(f"{digest}  {versioned.name}\n{digest}  {fixed.name}\n"
                                         f"{files_digest}  {files.name}\n{bundle_digest}  {bundle.name}\n",
                                         encoding="ascii")
    console.print(Panel.fit(f"[bold]{bundle.relative_to(ROOT)}[/] {bundle.stat().st_size / 1e6:.1f} MB: the zip to hand "
                            f"out\nsha256 {bundle_digest}\n\n"
                            f"[bold]{versioned.relative_to(ROOT)}[/] (and {fixed.relative_to(ROOT)})\n"
                            f"{versioned.stat().st_size / 1e6:.1f} MB\nsha256 {digest}\n"
                            f"[bold]{files.relative_to(ROOT)}[/] (for install.sh) {files.stat().st_size / 1e6:.1f} MB",
                            title="Packaged", border_style="green"))


def _export_dll(ctx: click.Context, no_build: bool) -> None:
    """package --dll: the release Mirage.dll in dist/, versioned and under its own name, with its sha256."""
    if not no_build and _build(ctx, "release", ("Mirage",)).returncode != 0:
        ctx.exit(1)
    src = _artifact("release", "Mirage.dll")
    if not src.exists():
        console.print("[red]No release Mirage.dll yet.[/] Run without --no-build.")
        ctx.exit(1)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    versioned = dist / f"Mirage-{project_version()}.dll"
    fixed = dist / "Mirage.dll"
    shutil.copy2(src, versioned)
    shutil.copy2(src, fixed)
    digest = hashlib.sha256(versioned.read_bytes()).hexdigest()
    # Its own sums file: SHA256SUMS.txt lists the release files and is rewritten by a full package
    (dist / "SHA256SUMS-dll.txt").write_text(f"{digest}  {versioned.name}\n{digest}  {fixed.name}\n", encoding="ascii")
    console.print(Panel.fit(f"[bold]{versioned.relative_to(ROOT)}[/] (and {fixed.relative_to(ROOT)})\n"
                            f"{versioned.stat().st_size / 1e6:.1f} MB\nsha256 {digest}",
                            title="DLL exported", border_style="green"))


def _crlf(text: str) -> str:
    """Windows line endings: the zip's text files get opened in Notepad."""
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


def _source_dirs(config: str) -> dict[str, Path]:
    """Where CMake fetched each dependency (from the build folder's CMakeCache.txt)."""
    out: dict[str, Path] = {}
    cache = b.build_dir(config) / "CMakeCache.txt"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"(?:CPM_PACKAGE_)?(\w+?)_SOURCE_DIR:\w+=(.+)", line)
        if m:
            out.setdefault(m.group(1).lower(), Path(m.group(2)))
    return out


def _third_party_notices(config: str) -> str:
    """THIRD-PARTY-NOTICES.txt: the license of everything compiled into the menu, from the sources it was built from."""
    dirs = _source_dirs(config)
    parts = [f"Mirage {project_version()} includes the following third-party software. Each is used under "
             "the license below.\n"]

    def section(title: str, what: str, url: str, text: str) -> None:
        parts.append("=" * 100 + f"\n{title}: {what}\n{url}\n" + "=" * 100 + "\n\n" + text.replace("﻿", "").strip() + "\n")   # no BOMs

    for name, what, url, package, files in THIRD_PARTY:
        if package in {"unreallib", "zlib"} and "unreallib" not in dirs:
            continue
        src = dirs.get(package.lower())
        if package == "fmt" and src is None:
            src = dirs.get("spdlog")
            files = ["include/spdlog/fmt/bundled/fmt.license.rst", "include/spdlog/fmt/bundled/LICENSE.rst"]
            name = "{fmt} (bundled with spdlog)"
        if src is None and package == "imgui_filebrowser":
            continue  # CMake fetches the file browser only when the TAS feature is enabled.
        if src is None:
            raise FileNotFoundError(f"{package} isn't in {b.build_dir(config).name}/CMakeCache.txt (build it first)")
        found = next((src / f for f in files if (src / f).is_file()), None)
        if found is None:
            raise FileNotFoundError(f"no license file for {name} in {src} (looked for {', '.join(files)})")
        section(name, what, url, found.read_text(encoding="utf-8", errors="replace"))

    # The menu's font comes with Dear ImGui's sources; its license text isn't there, so it ships with these tools.
    section("Roboto Medium", "the menu's font, by Christian Robertson for Google", "https://fonts.google.com/specimen/Roboto",
            "Licensed under the Apache License, Version 2.0:\n\n" +
            (DIST_FILES / "Apache-2.0.txt").read_text(encoding="utf-8"))
    section("Cousine", "the console's monospace font, by Steve Matteson", "https://fonts.google.com/specimen/Cousine",
            "Licensed under the Apache License, Version 2.0:\n\n" +
            (DIST_FILES / "Apache-2.0.txt").read_text(encoding="utf-8"))
    section("Mirror's Edge SDK", "game class layouts (generated headers)", "https://github.com/connorfrancey/mmultiplayer",
            "Generated from the game by the mmultiplayer project. Its repository has no license file.")
    return "\n".join(parts)


from . import devmode as _devmode   # noqa: E402 - registers `mirage dev ...` on the group above

_devmode.register(cli, console)

from . import modtools as _modtools   # noqa: E402 - registers `mirage new`, `mirage mod ...`, `mirage docs ...`



def _find_game_dir():
    from . import gametest as _gt
    return _gt.find_game_dir()


_modtools.register(cli, console, _toolchain, _find_game_dir)


def _early_dev_flag(argv: list[str]) -> None:
    """--dev / --no-dev before the command: developer mode has to be known before the commands are set up (it shows
    `mirage dev`, the interactive menu's developer tools, the tracebacks), so it goes into MIRAGE_DEV right away."""
    for arg in argv:
        if not arg.startswith("-"):
            return              # The command: options after it are its own
        if arg == "--dev":
            os.environ["MIRAGE_DEV"] = "1"
        elif arg == "--no-dev":
            os.environ.pop("MIRAGE_DEV", None)


# Need Windows itself (the D3D9 test host, the installer and launcher exes, the minimal build's MSVC presets). On Linux
# the game runs in Proton: install with install.sh, test with game-test.
_WINDOWS_ONLY = {"verbose-build", "install-game", "uninstall-game", "launch", "dev-test", "package", "minimal"}

cli.add_command(api_commands)


def main() -> None:
    if sys.platform != "win32" and not sys.platform.startswith("linux"):
        Console(stderr=True).print("[red]This project builds on Windows and Linux (see docs/linux-development.md).[/]")
        sys.exit(2)
    if sys.platform != "win32":
        command = next((a for a in sys.argv[1:] if not a.startswith("-")), "")
        if command in _WINDOWS_ONLY:
            Console(stderr=True).print(f"[red]`mirage {command}` needs Windows.[/] On Linux: build, test, errors and "
                                       "game-test work; install into the game with install.sh (see "
                                       "docs/linux-development.md).")
            sys.exit(2)
    _early_dev_flag(sys.argv[1:])
    if _devmode.enabled():
        _devmode.setup(console)
    cli(obj={})
