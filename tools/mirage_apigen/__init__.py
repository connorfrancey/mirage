"""MirageAPIGen development tools."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT = ROOT / "tools/sdkgen/engine-layouts.json"
DEFAULT_OUTPUT = ROOT / "build/api"
DEFAULT_CONFIG = ROOT / "tools/apigen/config/api.toml"
MERGED_INPUT = ROOT / "build/sdk-metadata.json"


def default_input() -> Path:
    return MERGED_INPUT if MERGED_INPUT.is_file() else DEFAULT_INPUT
