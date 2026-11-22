"""Dependency-free CMake entry point for the bundled reviewed bindings."""

from __future__ import annotations

import sys
from pathlib import Path

from . import DEFAULT_CONFIG, DEFAULT_INPUT
from .artifacts import apply_plan, output_lock, plan_output
from .pipeline import generate


def main() -> None:
    output = Path(sys.argv[1]).resolve()
    result = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("cpp",))
    if any(d.severity == "error" or d.code == "unsafe-binding" for d in result.api.diagnostics):
        sys.exit("MirageAPIGen: bundled policy failed validation")
    with output_lock(output):
        apply_plan(output, plan_output(output, result.files, result.metadata))


if __name__ == "__main__":
    main()
