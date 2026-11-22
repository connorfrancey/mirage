import gzip
import json
import os
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import click
from click.testing import CliRunner

from mirage_tools import tool_logging


@click.group(cls=tool_logging.LoggedGroup)
def tools():
    pass


@tools.group()
def api():
    pass


@api.command()
def inspect():
    click.echo('{"ok": true}')


@api.command()
def fail():
    raise click.ClickException("manifest missing")


class ToolLoggingTests(unittest.TestCase):
    def test_disabled_leaves_stdout_and_stderr_unchanged(self):
        with TemporaryDirectory() as folder, patch.dict(os.environ, {"MIRAGE_ENABLE_LOGGING": "false"}):
            path = Path(folder) / "logs" / "mirage-tools.log"
            with patch.object(tool_logging, "LOG_PATH", path):
                result = CliRunner().invoke(tools, ["api", "inspect"])
            self.assertEqual(result.exit_code, 0, result.output)
            self.assertEqual(json.loads(result.stdout), {"ok": True})
            self.assertEqual(result.stderr, "")
            self.assertFalse(path.parent.exists())

    def test_enabled_logs_to_stderr_and_file(self):
        with TemporaryDirectory() as folder, patch.dict(os.environ, {"MIRAGE_ENABLE_LOGGING": "yes"}):
            path = Path(folder) / "mirage-tools.log"
            with patch.object(tool_logging, "LOG_PATH", path):
                runner = CliRunner()
                result = runner.invoke(tools, ["api", "inspect"])
                failure = runner.invoke(tools, ["api", "fail"])
            self.assertEqual(result.exit_code, 0, result.output)
            self.assertEqual(json.loads(result.stdout), {"ok": True})
            self.assertIn("tools api inspect exited with 0", result.stderr)
            self.assertEqual(failure.exit_code, 1, failure.output)
            self.assertIn("manifest missing", failure.stderr)
            self.assertIn("tools api fail exited with 1", path.read_text(encoding="utf-8"))

    def test_rotation_keeps_active_log_and_compresses_old_content(self):
        with TemporaryDirectory() as folder, patch.dict(os.environ, {"MIRAGE_ENABLE_LOGGING": "1"}):
            path = Path(folder) / "mirage-tools.log"
            previous = "original log\n" * 40
            path.write_text(previous, encoding="utf-8")
            with patch.object(tool_logging, "LOG_PATH", path), patch.object(tool_logging, "ROTATION", 200):
                result = CliRunner().invoke(tools, ["api", "inspect"])
            self.assertEqual(result.exit_code, 0, result.output)
            archive, = Path(folder).glob("*.log.gz")
            self.assertEqual(gzip.decompress(archive.read_bytes()).decode(), previous.replace("\n", os.linesep))
            self.assertIn("tools api inspect exited with 0", path.read_text(encoding="utf-8"))
