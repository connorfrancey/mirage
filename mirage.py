#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["click>=8.1", "loguru>=0.8.0", "questionary>=2.1.1", "rich>=13.7", "zstandard>=0.23; python_version < '3.14'"]
# ///
"""Project manager for Mirage: build, test, clean, install into the game, package.

    python mirage.py                        interactive menu: configure and build from a list
    python mirage.py --help
    python mirage.py build                  debug build of Mirage.dll
    python mirage.py build -c release       release build
    python mirage.py build --with tests     also build an optional part (tests, installer, launcher)
    python mirage.py dev-test               build, start the game with the menu injected, report crashes
    python mirage.py minimal                only the D3D9 hook + ImGui demo window (INSERT)
    python mirage.py test                   build and run the unit tests
    python mirage.py verbose-build          build stage by stage, test every stage without the game
    python mirage.py clean --all            delete every build folder
    python mirage.py package                release installer -> dist/

`uv run mirage.py ...` works without any setup (uv installs click, rich and questionary from the header above).
With plain `python`, install them first: `pip install -r requirements.txt` (or `uv sync` and use the .venv).
questionary is only needed for the interactive menu.

This is a thin entry point: the commands live in tools/mirage_tools (also installed as `uv run mirage`).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))

try:
    from mirage_tools.cli import main
except ImportError as e:
    if e.name not in ("click", "rich"):
        raise
    sys.exit(f"mirage.py needs click and rich ({e.name} is missing).\n"
             f"Run it with `uv run mirage.py ...`, or install them: "
             f"{Path(sys.executable).name} -m pip install -r requirements.txt")

if __name__ == "__main__":
    main()
