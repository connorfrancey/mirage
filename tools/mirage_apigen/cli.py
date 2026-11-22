"""Click entry points for Mirage and standalone use."""

from __future__ import annotations

import cProfile
import functools
import io
import json
import pstats
from contextlib import nullcontext
from dataclasses import asdict
from pathlib import Path

import click

from . import DEFAULT_CONFIG, DEFAULT_OUTPUT, default_input
from .artifacts import apply_plan, json_text, output_lock, plan_output, previous_manifest
from .pipeline import TARGETS, Generation, generate

FILE = click.Path(dir_okay=False, path_type=Path)
DIRECTORY = click.Path(file_okay=False, path_type=Path)


def _errors(command):
    @functools.wraps(command)
    def run(*args, **kwargs):
        try:
            return command(*args, **kwargs)
        except (OSError, ValueError, RecursionError) as exc:
            raise click.ClickException(str(exc)) from exc
    return run


def _options(command):
    for option in (
        click.option("--input", "input_path", type=FILE, default=default_input,
                     show_default="build/sdk-metadata.json, then tools/sdkgen/engine-layouts.json"),
        click.option("--output", type=DIRECTORY, default=DEFAULT_OUTPUT, show_default=True),
        click.option("--config", type=FILE, default=DEFAULT_CONFIG, show_default=True),
        click.option("--package", default="", help="Select a package by its SDK name."),
        click.option("--class", "cls", default="", help="Select a fully qualified class, struct or enum."),
        click.option("--strict", is_flag=True, help="Treat diagnostic warnings as errors."),
        click.option("--verbose", is_flag=True, help="Show diagnostics and timings."),
    ):
        command = option(command)
    return command


def _report(result: Generation, strict: bool, verbose: bool) -> None:
    errors = sum(d.severity == "error" for d in result.api.diagnostics)
    warnings = sum(d.severity == "warning" for d in result.api.diagnostics)
    for d in result.api.diagnostics:
        if verbose or d.severity == "error":
            click.echo(f"{d.severity} {d.code} {d.subject}: {d.message}", err=True)
    click.echo(f"{len(result.api.types)} types, {len(result.api.members)} members, {warnings} warnings, {errors} errors.")
    if verbose:
        click.echo(" ".join(f"{phase}={seconds:.3f}s" for phase, seconds in result.timings.items()))
    if errors or (strict and warnings):
        raise click.ClickException("API validation failed.")


@click.group(context_settings={"help_option_names": ["-h", "--help"]})
def api() -> None:
    """Generate and inspect Mirage's scripting API from SDK layout JSON."""


@api.command("generate")
@_options
@click.option("--target", type=click.Choice(TARGETS), multiple=True)
@click.option("--all", "all_targets", is_flag=True, help="Generate every target (the default).")
@click.option("--inspection", is_flag=True, help="Include experimental SDK inspection definitions and reference pages.")
@click.option("--dry-run", is_flag=True)
@click.option("--force", is_flag=True)
@click.option("--clean", "clean_generation", is_flag=True, help="Regenerate all files and remove obsolete outputs.")
@click.option("--profile", is_flag=True, help="Show Python profiling statistics.")
@_errors
def generate_command(input_path: Path, output: Path, config: Path, package: str, cls: str, strict: bool,
                     verbose: bool, target: tuple[str, ...], all_targets: bool, inspection: bool, dry_run: bool,
                     force: bool, clean_generation: bool, profile: bool) -> None:
    """Generate LuaLS, C++ bindings and Markdown documentation."""
    if all_targets and target:
        raise click.UsageError("Choose either --all or --target.")
    profiler = cProfile.Profile() if profile else None
    if profiler:
        profiler.enable()
    result = generate(input_path, config, tuple(sorted(set(target))) or TARGETS, package=package, cls=cls,
                      include_inspection=inspection)
    if profiler:
        profiler.disable()
        stream = io.StringIO()
        pstats.Stats(profiler, stream=stream).sort_stats("cumulative").print_stats(15)
        click.echo(stream.getvalue())
    _report(result, strict, verbose)
    root = output.resolve()
    with nullcontext() if dry_run else output_lock(root):
        plan = plan_output(root, result.files, result.metadata, force=force or clean_generation)
        if verbose or dry_run:
            for name in plan.writes:
                click.echo(f"write {name}")
            for name in plan.removes:
                click.echo(f"remove {name}")
        click.echo(f"{len(plan.writes)} files to write, {len(plan.removes)} obsolete files.")
        if not dry_run:
            apply_plan(root, plan)


@api.command("validate")
@_options
@click.option("--json", "machine", is_flag=True, help="Print diagnostics and stale files as JSON.")
@_errors
def validate(input_path: Path, output: Path, config: Path, package: str, cls: str, strict: bool,
             verbose: bool, machine: bool) -> None:
    """Check metadata, exposure rules, and existing generated outputs."""
    root = output.resolve()
    previous = previous_manifest(root)
    targets = previous.get("targets", list(TARGETS))
    if (not isinstance(targets, list) or not targets or
            any(not isinstance(target, str) or target not in TARGETS for target in targets)):
        raise ValueError("Invalid targets in generation manifest")
    package = package or previous.get("package", "")
    cls = cls or previous.get("class", "")
    if not isinstance(package, str) or not isinstance(cls, str):
        raise ValueError("Invalid SDK selection in generation manifest")
    result = generate(input_path, config, tuple(targets), package=package, cls=cls,
                      include_inspection=previous.get("inspection") is True)
    failed = any(d.severity == "error" or (strict and d.severity == "warning") for d in result.api.diagnostics)
    stale: list[str] = []
    if previous and not failed:
        plan = plan_output(root, result.files, result.metadata, check_only=True)
        stale = [*plan.writes, *plan.removes]
    if machine:
        report = {"valid": not failed and not stale, "stale": stale,
                  "diagnostics": [asdict(d) for d in result.api.diagnostics]}
        if verbose:
            report["timings"] = result.timings
        click.echo(json_text(report), nl=False)
        if failed or stale:
            raise click.exceptions.Exit(1)
        return
    _report(result, strict, verbose)
    if stale:
        raise click.ClickException(f"Stale outputs: {', '.join(stale)}")
    if previous:
        click.echo("Generated files are current.")


@api.command("coverage")
@_options
@click.option("--json", "machine", is_flag=True)
@_errors
def coverage(input_path: Path, output: Path, config: Path, package: str, cls: str, strict: bool,
             verbose: bool, machine: bool) -> None:
    """Report coverage of reviewed runtime bindings and SDK inspection."""
    result = generate(input_path, config, (), package=package, cls=cls)
    if machine:
        click.echo(json_text(result.coverage), nl=False)
        if verbose:
            for d in result.api.diagnostics:
                click.echo(f"{d.severity} {d.code} {d.subject}: {d.message}", err=True)
        if any(d.severity == "error" or (strict and d.severity == "warning") for d in result.api.diagnostics):
            raise click.exceptions.Exit(1)
        return
    _report(result, strict, verbose)
    for scope in ("runtime", "editor"):
        click.echo(f"{scope}: " + ", ".join(f"{key}={value}" for key, value in result.coverage[scope].items()))


@api.command("inspect")
@click.argument("path")
@_options
@_errors
def inspect(input_path: Path, output: Path, config: Path, package: str, cls: str, strict: bool,
            verbose: bool, path: str) -> None:
    """Show exposure and binding information for a fully qualified type."""
    if cls and cls != path:
        raise click.UsageError("--class must match the inspected type.")
    result = generate(input_path, config, TARGETS, package=package, cls=path, include_inspection=True)
    _report(result, strict, verbose)
    click.echo(json_text({"type": result.api.types[path], "coverage": result.coverage,
                          "members": result.api.members}), nl=False)


@api.command("clean")
@click.option("--output", type=DIRECTORY, default=DEFAULT_OUTPUT, show_default=True)
@click.option("--dry-run", is_flag=True)
@click.option("--force", is_flag=True)
@_errors
def clean(output: Path, dry_run: bool, force: bool) -> None:
    """Remove files tracked by the generation manifest."""
    root = output.resolve()
    if not root.exists():
        return
    with nullcontext() if dry_run else output_lock(root):
        plan = plan_output(root, {}, {}, clean=True, force=force)
        for name in plan.removes:
            click.echo(f"remove {name}")
        if not dry_run:
            apply_plan(root, plan)


@api.command("diff")
@click.argument("old", type=FILE)
@click.argument("new", type=FILE)
@click.option("--json", "machine", is_flag=True)
@click.option("--strict", is_flag=True, help="Fail when the comparison finds breaking changes.")
@_errors
def diff(old: Path, new: Path, machine: bool, strict: bool) -> None:
    """Compare two generated API manifests or two SDK JSON files."""
    from .compat import compare

    result = compare(old, new)
    if machine:
        click.echo(json_text(result), nl=False)
    else:
        for change in result["changes"]:
            suffix = " (breaking)" if change["breaking"] else ""
            click.echo(f"{change['change']} {change['name']}{suffix}")
        click.echo(f"{len(result['changes'])} changes.")
    if strict and result["breaking"]:
        raise click.ClickException("Breaking API changes detected.")


if __name__ == "__main__":
    api()
